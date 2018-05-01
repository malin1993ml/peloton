//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// table_scan_translator.cpp
//
// Identification: src/codegen/operator/table_scan_translator.cpp
//
// Copyright (c) 2015-2017, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "codegen/operator/table_scan_translator.h"

#include "codegen/lang/if.h"
#include "codegen/proxy/executor_context_proxy.h"
#include "codegen/proxy/storage_manager_proxy.h"
#include "codegen/proxy/transaction_runtime_proxy.h"
#include "codegen/proxy/runtime_functions_proxy.h"
#include "codegen/proxy/zone_map_proxy.h"
#include "codegen/type/boolean_type.h"
#include "codegen/type/type.h"
#include "expression/comparison_expression.h"
#include "expression/constant_value_expression.h"
#include "expression/tuple_value_expression.h"
#include "planner/seq_scan_plan.h"
#include "storage/data_table.h"
#include "storage/zone_map_manager.h"

#include "llvm/IR/Module.h"

// #define ORIGINAL_ORDER

namespace peloton {
namespace codegen {

//===----------------------------------------------------------------------===//
// TABLE SCAN TRANSLATOR
//===----------------------------------------------------------------------===//

// Constructor
TableScanTranslator::TableScanTranslator(const planner::SeqScanPlan &scan,
                                         CompilationContext &context,
                                         Pipeline &pipeline)
    : OperatorTranslator(context, pipeline),
      scan_(scan),
      table_(*scan_.GetTable()) {
  LOG_DEBUG("Constructing TableScanTranslator ...");

  // The restriction, if one exists
  const auto *predicate = GetScanPlan().GetPredicate();
  if (predicate != nullptr) {
    // If there is a predicate, prepare a translator for it
    context.Prepare(*predicate);

    // If the scan's predicate is SIMDable, install a boundary at the output
    if (predicate->IsSIMDable()) {
      pipeline.InstallBoundaryAtOutput(this);
    }
  }

  for (const auto &simd_predicate : GetScanPlan().GetSIMDPredicates()) {
    if (simd_predicate != nullptr) {
      context.Prepare(*simd_predicate);
    }
  }

  const auto *non_simd_predicate = GetScanPlan().GetNonSIMDPredicate();
  if (non_simd_predicate != nullptr) {
    context.Prepare(*non_simd_predicate);
  }

  LOG_DEBUG("Finished constructing TableScanTranslator ...");
}

// Produce!
void TableScanTranslator::Produce() const {
  auto &codegen = GetCodeGen();
  auto &table = GetTable();

  LOG_TRACE("TableScan on [%u] starting to produce tuples ...", table.GetOid());

  // Get the table instance from the database
  llvm::Value *storage_manager_ptr = GetStorageManagerPtr();
  llvm::Value *db_oid = codegen.Const32(table.GetDatabaseOid());
  llvm::Value *table_oid = codegen.Const32(table.GetOid());
  llvm::Value *table_ptr =
      codegen.Call(StorageManagerProxy::GetTableWithOid,
                   {storage_manager_ptr, db_oid, table_oid});

  // The selection vector for the scan
  auto *raw_vec = codegen.AllocateBuffer(
      codegen.Int32Type(), Vector::kDefaultVectorSize, "scanSelVector");
  Vector sel_vec{raw_vec, Vector::kDefaultVectorSize, codegen.Int32Type()};

  auto predicate = const_cast<expression::AbstractExpression *>(
      GetScanPlan().GetPredicate());
  llvm::Value *predicate_ptr = codegen->CreateIntToPtr(
      codegen.Const64((int64_t)predicate),
      AbstractExpressionProxy::GetType(codegen)->getPointerTo());
  size_t num_preds = 0;

  auto *zone_map_manager = storage::ZoneMapManager::GetInstance();
  if (predicate != nullptr && zone_map_manager->ZoneMapTableExists()) {
    if (predicate->IsZoneMappable()) {
      num_preds = predicate->GetNumberofParsedPredicates();
    }
  }
  ScanConsumer scan_consumer{*this, sel_vec};
  table_.GenerateScan(codegen, table_ptr, sel_vec.GetCapacity(), scan_consumer,
                      predicate_ptr, num_preds);
  codegen.Call(TransactionRuntimeProxy::PrintClockDuration, {});
  LOG_TRACE("TableScan on [%u] finished producing tuples ...", table.GetOid());
}

// Get the stringified name of this scan
std::string TableScanTranslator::GetName() const {
  std::string name = "Scan('" + GetTable().GetName() + "'";
  auto *predicate = GetScanPlan().GetPredicate();
  if (predicate != nullptr && predicate->IsSIMDable()) {
    name.append(", ").append(std::to_string(Vector::kDefaultVectorSize));
  }
  name.append(")");
  return name;
}

// Table accessor
const storage::DataTable &TableScanTranslator::GetTable() const {
  return *scan_.GetTable();
}

//===----------------------------------------------------------------------===//
// VECTORIZED SCAN CONSUMER
//===----------------------------------------------------------------------===//

// Constructor
TableScanTranslator::ScanConsumer::ScanConsumer(
    const TableScanTranslator &translator, Vector &selection_vector)
    : translator_(translator), selection_vector_(selection_vector) {}

// Generate the body of the vectorized scan
void TableScanTranslator::ScanConsumer::ProcessTuples(
    CodeGen &codegen, llvm::Value *tid_start, llvm::Value *tid_end,
    TileGroup::TileGroupAccess &tile_group_access) {
  // TODO: Should visibility check be done here or in codegen::Table/TileGroup?

#ifdef ORIGINAL_ORDER
  // 1. Filter the rows in the range [tid_start, tid_end) by txn visibility
  FilterRowsByVisibility(codegen, tid_start, tid_end, selection_vector_);

  // 2. Filter rows by the given predicate (if one exists)
  auto *predicate = GetPredicate();
  if (predicate != nullptr) {
    // First perform a vectorized filter, putting TIDs into the selection vector
    FilterRowsByPredicate(codegen, tile_group_access, tid_start, tid_end,
                          selection_vector_);
  }
#else
  auto *predicate = GetPredicate();
  if (predicate != nullptr) {
    FilterRowsByPredicate(codegen, tile_group_access, tid_start, tid_end,
                          selection_vector_);
  } else {
    selection_vector_.SetNumElements(codegen.Const32(-1));
  }

  FilterRowsByVisibility(codegen, tid_start, tid_end, selection_vector_);
#endif

  // 3. Setup the (filtered) row batch and setup attribute accessors
  RowBatch batch{translator_.GetCompilationContext(), tile_group_id_, tid_start,
                 tid_end, selection_vector_, true};

  std::vector<TableScanTranslator::AttributeAccess> attribute_accesses;
  SetupRowBatch(batch, tile_group_access, attribute_accesses);

  // 4. Push the batch into the pipeline
  ConsumerContext context{translator_.GetCompilationContext(),
                          translator_.GetPipeline()};
  context.Consume(batch);
}

void TableScanTranslator::ScanConsumer::SetupRowBatch(
    RowBatch &batch, TileGroup::TileGroupAccess &tile_group_access,
    std::vector<TableScanTranslator::AttributeAccess> &access) const {
  // Grab a hold of the stuff we need (i.e., the plan, all the attributes, and
  // the IDs of the columns the scan _actually_ produces)
  const auto &scan_plan = translator_.GetScanPlan();
  std::vector<const planner::AttributeInfo *> ais;
  scan_plan.GetAttributes(ais);
  const auto &output_col_ids = scan_plan.GetColumnIds();

  // 1. Put all the attribute accessors into a vector
  access.clear();
  for (oid_t col_idx = 0; col_idx < output_col_ids.size(); col_idx++) {
    access.emplace_back(tile_group_access, ais[output_col_ids[col_idx]]);
  }

  // 2. Add the attribute accessors into the row batch
  for (oid_t col_idx = 0; col_idx < output_col_ids.size(); col_idx++) {
    auto *attribute = ais[output_col_ids[col_idx]];
    LOG_TRACE("Adding attribute '%s.%s' (%p) into row batch",
              scan_plan.GetTable()->GetName().c_str(), attribute->name.c_str(),
              attribute);
    batch.AddAttribute(attribute, &access[col_idx]);
  }
}

void TableScanTranslator::ScanConsumer::FilterRowsByVisibility(
    CodeGen &codegen, llvm::Value *tid_start, llvm::Value *tid_end,
    Vector &selection_vector) const {
  llvm::Value *executor_context_ptr =
      translator_.GetCompilationContext().GetExecutorContextPtr();
  llvm::Value *txn = codegen.Call(ExecutorContextProxy::GetTransaction,
                                  {executor_context_ptr});
  llvm::Value *raw_sel_vec = selection_vector.GetVectorPtr();

#ifdef ORIGINAL_ORDER
  // Invoke TransactionRuntime::PerformRead(...)
  llvm::Value *out_idx =
      codegen.Call(TransactionRuntimeProxy::PerformVectorizedRead,
                   {txn, tile_group_ptr_, tid_start, tid_end, raw_sel_vec, codegen.Const32(-1)});
#else
  llvm::Value *out_idx =
      codegen.Call(TransactionRuntimeProxy::PerformVectorizedRead,
                   {txn, tile_group_ptr_, tid_start, tid_end, raw_sel_vec, selection_vector.GetNumElements()});
#endif
  selection_vector.SetNumElements(out_idx);
}

// Get the predicate, if one exists
const expression::AbstractExpression *
TableScanTranslator::ScanConsumer::GetPredicate() const {
  return translator_.GetScanPlan().GetPredicate();
}

const std::vector<std::unique_ptr<expression::AbstractExpression>> &
TableScanTranslator::ScanConsumer::GetSIMDPredicates() const {
  return translator_.GetScanPlan().GetSIMDPredicates();
}

const expression::AbstractExpression *
TableScanTranslator::ScanConsumer::GetNonSIMDPredicate() const {
  return translator_.GetScanPlan().GetNonSIMDPredicate();
}

void TableScanTranslator::ScanConsumer::FilterRowsByPredicate(
    CodeGen &codegen, const TileGroup::TileGroupAccess &access,
    llvm::Value *tid_start, llvm::Value *tid_end,
    Vector &selection_vector) const {
  auto &compilation_ctx = translator_.GetCompilationContext();

  const auto &simd_predicates = GetSIMDPredicates();
  const auto *non_simd_predicate = GetNonSIMDPredicate();

  codegen.Call(TransactionRuntimeProxy::GetClockStart, {});

  if (simd_predicates.empty() && non_simd_predicate == nullptr) {
    non_simd_predicate = GetPredicate();
  }

  uint32_t N = 32;

#ifdef ORIGINAL_ORDER
  for (auto &simd_predicate : simd_predicates) {
    LOG_INFO("SIMD predicate detected");
    LOG_INFO("%s", simd_predicate->GetInfo().c_str());

    // The batch we're filtering
    RowBatch batch{compilation_ctx, tile_group_id_,   tid_start,
                   tid_end,         selection_vector, true};

    // Determine the attributes the predicate needs
    std::unordered_set<const planner::AttributeInfo *> used_attributes;
    simd_predicate->GetUsedAttributes(used_attributes);

    // Setup the row batch with attribute accessors for the predicate
    std::vector<AttributeAccess> attribute_accessors;
    for (const auto *ai : used_attributes) {
      attribute_accessors.emplace_back(access, ai);
    }
    for (uint32_t i = 0; i < attribute_accessors.size(); i++) {
      auto &accessor = attribute_accessors[i];
      batch.AddAttribute(accessor.GetAttributeRef(), &accessor);
    }

    auto *orig_size = batch.GetNumValidRows(codegen);
    auto *align_size = codegen->CreateMul(
        codegen.Const32(N), codegen->CreateUDiv(orig_size, codegen.Const32(N)));
    selection_vector.SetNumElements(align_size);

    batch.VectorizedIterate(codegen, N, [&](RowBatch::
                                                VectorizedIterateCallback::
                                                    IterationInstance &ins) {
      llvm::Value *final_pos = ins.write_pos;

      llvm::Value *lhs = nullptr;
      llvm::Value *rhs = nullptr;

      auto *exp_lch = simd_predicate->GetChild(0);
      auto *exp_rch = simd_predicate->GetChild(1);

      auto orig_typ_lch = type::Type(exp_lch->GetValueType(), exp_lch->IsNullable());
      auto orig_typ_rch = type::Type(exp_rch->GetValueType(), exp_rch->IsNullable());

      auto cast_typ_lch = orig_typ_lch;
      auto cast_typ_rch = orig_typ_rch;
      type::TypeSystem::GetComparison(orig_typ_lch, cast_typ_lch, orig_typ_rch, cast_typ_rch);
      cast_typ_lch.nullable = orig_typ_lch.nullable;
      cast_typ_rch.nullable = orig_typ_rch.nullable;

      llvm::Type *dummy, *cast_typ_lhs, *cast_typ_rhs;
      cast_typ_lch.GetSqlType().GetTypeForMaterialization(codegen, cast_typ_lhs, dummy);
      cast_typ_rch.GetSqlType().GetTypeForMaterialization(codegen, cast_typ_rhs, dummy);

      llvm::Value *lhs_is_null = nullptr;
      llvm::Value *rhs_is_null = nullptr;

      if (dynamic_cast<const expression::ConstantValueExpression *>(exp_lch) !=
          nullptr) {
        RowBatch::Row row = batch.GetRowAt(ins.start);
        codegen::Value eval_row = row.DeriveValue(codegen, *exp_lch);
        llvm::Value *ins_val = eval_row.CastTo(codegen, cast_typ_lch).GetValue();
        lhs = codegen->CreateVectorSplat(N, ins_val);
      } else {
        lhs = llvm::UndefValue::get(llvm::VectorType::get(cast_typ_lhs, N));
        lhs_is_null = llvm::UndefValue::get(llvm::VectorType::get(codegen.BoolType(), N));
        for (uint32_t i = 0; i < N; ++i) {
          RowBatch::Row row =
              batch.GetRowAt(codegen->CreateAdd(ins.start, codegen.Const32(i)));
          codegen::Value eval_row = row.DeriveValue(codegen, *exp_lch);
          auto cast_val = eval_row.CastTo(codegen, cast_typ_lch);
          lhs = codegen->CreateInsertElement(lhs, cast_val.GetValue(), i);
          lhs_is_null = codegen->CreateInsertElement(lhs_is_null, cast_val.IsNull(codegen), i);
        }
      }

      if (dynamic_cast<const expression::ConstantValueExpression *>(exp_rch) !=
          nullptr) {
        RowBatch::Row row = batch.GetRowAt(ins.start);
        codegen::Value eval_row = row.DeriveValue(codegen, *exp_rch);
        llvm::Value *ins_val = eval_row.CastTo(codegen, cast_typ_rch).GetValue();
        rhs = codegen->CreateVectorSplat(N, ins_val);
      } else {
        rhs = llvm::UndefValue::get(llvm::VectorType::get(cast_typ_lhs, N));
        rhs_is_null = llvm::UndefValue::get(llvm::VectorType::get(codegen.BoolType(), N));
        for (uint32_t i = 0; i < N; ++i) {
          RowBatch::Row row =
              batch.GetRowAt(codegen->CreateAdd(ins.start, codegen.Const32(i)));
          codegen::Value eval_row = row.DeriveValue(codegen, *exp_rch);
          auto cast_val = eval_row.CastTo(codegen, cast_typ_rch);
          rhs = codegen->CreateInsertElement(rhs, cast_val.GetValue(), i);
          rhs_is_null = codegen->CreateInsertElement(rhs_is_null, cast_val.IsNull(codegen), i);
        }
      }

      codegen::Value val_lhs{cast_typ_lch, lhs, nullptr, lhs_is_null};
      codegen::Value val_rhs{cast_typ_rch, rhs, nullptr, rhs_is_null};

      auto *exp_cmp = static_cast<const expression::ComparisonExpression *>(
          simd_predicate.get());
      Value comp_val;
      switch (exp_cmp->GetExpressionType()) {
        case ExpressionType::COMPARE_EQUAL:
          comp_val = val_lhs.CompareEq(codegen, val_rhs);
          break;
        case ExpressionType::COMPARE_NOTEQUAL:
          comp_val = val_lhs.CompareNe(codegen, val_rhs);
          break;
        case ExpressionType::COMPARE_LESSTHAN:
          comp_val = val_lhs.CompareLt(codegen, val_rhs);
          break;
        case ExpressionType::COMPARE_LESSTHANOREQUALTO:
          comp_val = val_lhs.CompareLte(codegen, val_rhs);
          break;
        case ExpressionType::COMPARE_GREATERTHAN:
          comp_val = val_lhs.CompareGt(codegen, val_rhs);
          break;
        case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
          comp_val = val_lhs.CompareGte(codegen, val_rhs);
          break;
        default:
          ;
      }

      for (uint32_t i = 0; i < N; ++i) {
        RowBatch::OutputTracker tracker{batch.GetSelectionVector(), final_pos};
        RowBatch::Row row = batch.GetRowAt(
            codegen->CreateAdd(ins.start, codegen.Const32(i)), &tracker);
        Value valid_row{comp_val.GetType(), codegen->CreateExtractElement(comp_val.GetValue(), i), nullptr, codegen->CreateExtractElement(comp_val.IsNull(codegen), i)};
        PELOTON_ASSERT(valid_row.GetType().GetSqlType() == type::Boolean::Instance());
        llvm::Value *bool_val =
            type::Boolean::Instance().Reify(codegen, valid_row);
        row.SetValidity(codegen, bool_val);
        final_pos = tracker.GetFinalOutputPos();
      }

      return final_pos;
    });

    auto pre_bb = codegen->GetInsertBlock();
    auto *check_post_batch_bb = llvm::BasicBlock::Create(
        codegen.GetContext(), "checkPostBatch", pre_bb->getParent());
    auto *loop_post_batch_bb = llvm::BasicBlock::Create(
        codegen.GetContext(), "loopPostBatch", pre_bb->getParent());
    auto *end_post_batch_bb = llvm::BasicBlock::Create(
        codegen.GetContext(), "endPostBatch", pre_bb->getParent());

    codegen->CreateBr(check_post_batch_bb);

    codegen->SetInsertPoint(check_post_batch_bb);
    auto *idx_cur = codegen->CreatePHI(align_size->getType(), 2);
    idx_cur->addIncoming(align_size, pre_bb);
    auto *write_pos =
        codegen->CreatePHI(selection_vector.GetNumElements()->getType(), 2);
    write_pos->addIncoming(selection_vector.GetNumElements(), pre_bb);
    auto *cond = codegen->CreateICmpULT(idx_cur, orig_size);
    codegen->CreateCondBr(cond, loop_post_batch_bb, end_post_batch_bb);

    codegen->SetInsertPoint(loop_post_batch_bb);
    {
      RowBatch::OutputTracker tracker{batch.GetSelectionVector(), write_pos};
      RowBatch::Row row = batch.GetRowAt(idx_cur, &tracker);

      codegen::Value valid_row = row.DeriveValue(codegen, *simd_predicate);

      // Reify the boolean value since it may be NULL
      PELOTON_ASSERT(valid_row.GetType().GetSqlType() ==
                     type::Boolean::Instance());
      llvm::Value *bool_val =
          type::Boolean::Instance().Reify(codegen, valid_row);

      row.SetValidity(codegen, bool_val);
      idx_cur->addIncoming(codegen->CreateAdd(idx_cur, codegen.Const32(1)),
                           codegen->GetInsertBlock());
      write_pos->addIncoming(tracker.GetFinalOutputPos(), codegen->GetInsertBlock());
      codegen->CreateBr(check_post_batch_bb);
    }

    codegen->SetInsertPoint(end_post_batch_bb);
    batch.UpdateWritePosition(write_pos);
  }
#else
  llvm::Value *align_start = tid_start;
  llvm::Value *orig_size = codegen->CreateSub(tid_end, tid_start);
  llvm::Value *align_size = codegen->CreateMul(
      codegen.Const32(N), codegen->CreateUDiv(orig_size, codegen.Const32(N)));
  llvm::Value *align_end = codegen->CreateAdd(tid_start, align_size);

  // The batch we're filtering
  RowBatch batch{compilation_ctx, tile_group_id_,   align_start,
                 align_end,       selection_vector, false};

  // Determine the attributes the predicate needs
  std::unordered_set<const planner::AttributeInfo *> used_attributes;
  GetPredicate()->GetUsedAttributes(used_attributes);

  // Setup the row batch with attribute accessors for the predicate
  std::vector<AttributeAccess> attribute_accessors;
  for (const auto *ai : used_attributes) {
    attribute_accessors.emplace_back(access, ai);
  }
  for (uint32_t i = 0; i < attribute_accessors.size(); i++) {
    auto &accessor = attribute_accessors[i];
    batch.AddAttribute(accessor.GetAttributeRef(), &accessor);
  }

  batch.VectorizedIterate(codegen, N, [&](RowBatch::
                                          VectorizedIterateCallback::
                                          IterationInstance &ins) {
    llvm::Value *final_pos = ins.write_pos;

    llvm::Value *mask = llvm::Constant::getAllOnesValue(llvm::VectorType::get(codegen.BoolType(), N));

    for (auto &simd_predicate : simd_predicates) {
      llvm::Value *lhs = nullptr;
      llvm::Value *rhs = nullptr;

      auto *exp_lch = simd_predicate->GetChild(0);
      auto *exp_rch = simd_predicate->GetChild(1);

      auto orig_typ_lch = type::Type(exp_lch->GetValueType(), exp_lch->IsNullable());
      auto orig_typ_rch = type::Type(exp_rch->GetValueType(), exp_rch->IsNullable());

      llvm::Type *dummy, *orig_typ_lhs, *orig_typ_rhs;
      orig_typ_lch.GetSqlType().GetTypeForMaterialization(codegen, orig_typ_lhs, dummy);
      orig_typ_rch.GetSqlType().GetTypeForMaterialization(codegen, orig_typ_rhs, dummy);

      auto cast_typ_lch = orig_typ_lch;
      auto cast_typ_rch = orig_typ_rch;
      type::TypeSystem::GetComparison(orig_typ_lch, cast_typ_lch, orig_typ_rch, cast_typ_rch);
      cast_typ_lch.nullable = orig_typ_lch.nullable;
      cast_typ_rch.nullable = orig_typ_rch.nullable;

      llvm::Type *cast_typ_lhs, *cast_typ_rhs;
      cast_typ_lch.GetSqlType().GetTypeForMaterialization(codegen, cast_typ_lhs, dummy);
      cast_typ_rch.GetSqlType().GetTypeForMaterialization(codegen, cast_typ_rhs, dummy);

      llvm::Value *lhs_is_null = nullptr;
      llvm::Value *rhs_is_null = nullptr;

      if (dynamic_cast<const expression::ConstantValueExpression *>(exp_lch) !=
          nullptr) {
        RowBatch::Row row = batch.GetRowAt(ins.start);
        codegen::Value eval_row = row.DeriveValue(codegen, *exp_lch);
        llvm::Value *ins_val = eval_row.CastTo(codegen, cast_typ_lch).GetValue();
        lhs = codegen->CreateVectorSplat(N, ins_val);
      } else {
        auto *tve = static_cast<const expression::TupleValueExpression *>(exp_lch);
        auto *ai = tve->GetAttributeRef();

        RowBatch::Row first_row = batch.GetRowAt(ins.start);
        llvm::Value *ptr = first_row.DeriveFixedLengthPtrInTableScan(codegen, ai);
        ptr = codegen->CreateBitCast(ptr, llvm::VectorType::get(orig_typ_lhs, N)->getPointerTo());

        auto *uncasted_lhs = codegen->CreateMaskedLoad(ptr, 0, mask);
        if (orig_typ_lch.nullable) {
          auto &sql_type = orig_typ_lch.GetSqlType();
          auto val_tmp = Value{sql_type, uncasted_lhs};
          auto null_val = Value{sql_type, codegen->CreateVectorSplat(N, sql_type.GetNullValue(codegen).GetValue())};
          auto val_is_null = val_tmp.CompareEq(codegen, null_val);
          llvm::Value *is_null = val_is_null.GetValue();
          auto cast_val = codegen::Value{orig_typ_lch, uncasted_lhs, nullptr, is_null}.CastTo(codegen, cast_typ_lch);
          lhs = cast_val.GetValue();
          lhs_is_null = is_null;
        } else {
          lhs = codegen::Value{orig_typ_lch, uncasted_lhs}.CastTo(codegen, cast_typ_lch).GetValue();
        }
      }

      if (dynamic_cast<const expression::ConstantValueExpression *>(exp_rch) !=
          nullptr) {
        RowBatch::Row row = batch.GetRowAt(ins.start);
        codegen::Value eval_row = row.DeriveValue(codegen, *exp_rch);
        llvm::Value *ins_val = eval_row.CastTo(codegen, cast_typ_rch).GetValue();
        rhs = codegen->CreateVectorSplat(N, ins_val);
      } else {
        auto *tve = static_cast<const expression::TupleValueExpression *>(exp_rch);
        auto *ai = tve->GetAttributeRef();

        RowBatch::Row first_row = batch.GetRowAt(ins.start);
        llvm::Value *ptr = first_row.DeriveFixedLengthPtrInTableScan(codegen, ai);
        ptr = codegen->CreateBitCast(ptr, llvm::VectorType::get(orig_typ_rhs, N)->getPointerTo());

        auto *uncasted_rhs = codegen->CreateMaskedLoad(ptr, 0, mask);
        if (orig_typ_rch.nullable) {
          auto &sql_type = orig_typ_rch.GetSqlType();
          auto val_tmp = Value{sql_type, uncasted_rhs};
          auto null_val = Value{sql_type, codegen->CreateVectorSplat(N, sql_type.GetNullValue(codegen).GetValue())};
          auto val_is_null = val_tmp.CompareEq(codegen, null_val);
          llvm::Value *is_null = val_is_null.GetValue();
          auto cast_val = codegen::Value{orig_typ_rch, uncasted_rhs, nullptr, is_null}.CastTo(codegen, cast_typ_rch);
          rhs = cast_val.GetValue();
          rhs_is_null = is_null;
        } else {
          rhs = codegen::Value{orig_typ_rch, uncasted_rhs}.CastTo(codegen, cast_typ_rch).GetValue();
        }
      }

      codegen::Value val_lhs{cast_typ_lch, lhs, nullptr, lhs_is_null};
      codegen::Value val_rhs{cast_typ_rch, rhs, nullptr, rhs_is_null};

      auto *exp_cmp = static_cast<const expression::ComparisonExpression *>(
          simd_predicate.get());
      Value comp_val;
      switch (exp_cmp->GetExpressionType()) {
        case ExpressionType::COMPARE_EQUAL:
          comp_val = val_lhs.CompareEq(codegen, val_rhs);
          break;
        case ExpressionType::COMPARE_NOTEQUAL:
          comp_val = val_lhs.CompareNe(codegen, val_rhs);
          break;
        case ExpressionType::COMPARE_LESSTHAN:
          comp_val = val_lhs.CompareLt(codegen, val_rhs);
          break;
        case ExpressionType::COMPARE_LESSTHANOREQUALTO:
          comp_val = val_lhs.CompareLte(codegen, val_rhs);
          break;
        case ExpressionType::COMPARE_GREATERTHAN:
          comp_val = val_lhs.CompareGt(codegen, val_rhs);
          break;
        case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
          comp_val = val_lhs.CompareGte(codegen, val_rhs);
          break;
        default:
          ;
      }

      PELOTON_ASSERT(comp_val.GetType().GetSqlType() == type::Boolean::Instance());
      auto bool_val = type::Boolean::Instance().Reify(codegen, comp_val);

      mask = codegen->CreateAnd(mask, bool_val);
    }

    for (uint32_t i = 0; i < N; ++i) {
      RowBatch::OutputTracker tracker{batch.GetSelectionVector(), final_pos};
      RowBatch::Row row = batch.GetRowAt(
          codegen->CreateAdd(ins.start, codegen.Const32(i)), &tracker);
      row.SetValidity(codegen, codegen->CreateExtractElement(mask, i));
      final_pos = tracker.GetFinalOutputPos();
    }

    return final_pos;
  });

  batch.SetFiltered(false);

  auto pre_bb = codegen->GetInsertBlock();
  auto *check_post_batch_bb = llvm::BasicBlock::Create(
      codegen.GetContext(), "checkPostBatch", pre_bb->getParent());
  auto *loop_post_batch_bb = llvm::BasicBlock::Create(
      codegen.GetContext(), "loopPostBatch", pre_bb->getParent());
  auto *end_post_batch_bb = llvm::BasicBlock::Create(
      codegen.GetContext(), "endPostBatch", pre_bb->getParent());

  codegen->CreateBr(check_post_batch_bb);

  codegen->SetInsertPoint(check_post_batch_bb);
  auto *idx_cur = codegen->CreatePHI(align_size->getType(), 2);
  idx_cur->addIncoming(align_size, pre_bb);
  auto *write_pos =
      codegen->CreatePHI(selection_vector.GetNumElements()->getType(), 2);
  write_pos->addIncoming(selection_vector.GetNumElements(), pre_bb);
  auto *cond = codegen->CreateICmpULT(idx_cur, orig_size);
  codegen->CreateCondBr(cond, loop_post_batch_bb, end_post_batch_bb);

  codegen->SetInsertPoint(loop_post_batch_bb);
  {
    RowBatch::OutputTracker tracker{batch.GetSelectionVector(), write_pos};
    RowBatch::Row row = batch.GetRowAt(idx_cur, &tracker);
    llvm::Value *mask = codegen.ConstBool(true);

    for (auto &simd_predicate : simd_predicates) {
      codegen::Value valid_row = row.DeriveValue(codegen, *simd_predicate);
      PELOTON_ASSERT(valid_row.GetType().GetSqlType() ==
                     type::Boolean::Instance());
      llvm::Value *bool_val =
          type::Boolean::Instance().Reify(codegen, valid_row);
      mask = codegen->CreateAnd(mask, bool_val);
    }

    row.SetValidity(codegen, mask);
    idx_cur->addIncoming(codegen->CreateAdd(idx_cur, codegen.Const32(1)),
                         codegen->GetInsertBlock());
    write_pos->addIncoming(tracker.GetFinalOutputPos(), codegen->GetInsertBlock());
    codegen->CreateBr(check_post_batch_bb);
  }

  codegen->SetInsertPoint(end_post_batch_bb);
  batch.UpdateWritePosition(write_pos);
#endif

  if (non_simd_predicate != nullptr) {
    // The batch we're filtering
    RowBatch batch{compilation_ctx, tile_group_id_,   tid_start,
                   tid_end,         selection_vector, true};

    // Determine the attributes the predicate needs
    std::unordered_set<const planner::AttributeInfo *> used_attributes;
    non_simd_predicate->GetUsedAttributes(used_attributes);

    // Setup the row batch with attribute accessors for the predicate
    std::vector<AttributeAccess> attribute_accessors;
    for (const auto *ai : used_attributes) {
      attribute_accessors.emplace_back(access, ai);
    }
    for (uint32_t i = 0; i < attribute_accessors.size(); i++) {
      auto &accessor = attribute_accessors[i];
      batch.AddAttribute(accessor.GetAttributeRef(), &accessor);
    }

    // Iterate over the batch using a scalar loop
    batch.Iterate(codegen, [&](RowBatch::Row &row) {
      // Evaluate the predicate to determine row validity
      codegen::Value valid_row = row.DeriveValue(codegen, *non_simd_predicate);

      // Reify the boolean value since it may be NULL
      PELOTON_ASSERT(valid_row.GetType().GetSqlType() ==
                     type::Boolean::Instance());
      llvm::Value *bool_val =
          type::Boolean::Instance().Reify(codegen, valid_row);

      // Set the validity of the row
      row.SetValidity(codegen, bool_val);
    });
  }

  codegen.Call(TransactionRuntimeProxy::GetClockPause, {});
}

//===----------------------------------------------------------------------===//
// ATTRIBUTE ACCESS
//===----------------------------------------------------------------------===//

TableScanTranslator::AttributeAccess::AttributeAccess(
    const TileGroup::TileGroupAccess &access, const planner::AttributeInfo *ai)
    : tile_group_access_(access), ai_(ai) {}

codegen::Value TableScanTranslator::AttributeAccess::Access(
    CodeGen &codegen, RowBatch::Row &row) {
  auto raw_row = tile_group_access_.GetRow(row.GetTID(codegen));
  return raw_row.LoadColumn(codegen, ai_->attribute_id);
}

llvm::Value *TableScanTranslator::AttributeAccess::GetFixedLengthPtr(peloton::codegen::CodeGen &codegen,
                                                            peloton::codegen::RowBatch::Row &row) {
  auto raw_row = tile_group_access_.GetRow(row.GetTID(codegen));
  return raw_row.GetFixedLengthColumnPtr(codegen, ai_->attribute_id);
}

}  // namespace codegen
}  // namespace peloton
