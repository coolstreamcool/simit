#include "lower.h"

#include "ir_rewriter.h"

#include <set>

#include "ir.h"

#include "ir_rewriter.h"
#include "domain.h"
#include "usedef.h"
#include "sig.h"
#include "indexvar.h"
#include "util.h"
#include "ir_builder.h"
#include "error.h"
#include "ir_queries.h"
#include "substitute.h"
#include "tensor_storage.h"

#include "flatten.h"

using namespace std;

namespace simit {
namespace ir {

class LoopVars : public SIGVisitor {
public:
  LoopVars() : ud(nullptr) {}
  LoopVars(const SIG &sig, const UseDef *ud) : ud(ud) {apply(sig);}

  const pair<Var,ForDomain> &getVar(const IndexVar &var) const {
    return vertexLoopVars.at(var);
  }

  const pair<Var,ForDomain> &getVar(const Var &tensor) const {
    return edgeLoopVars.at(tensor);
  }

  bool hasVar(const Var &tensor) const {
    return edgeLoopVars.find(tensor) != edgeLoopVars.end();
  }

  friend std::ostream &operator<<(std::ostream &os, const LoopVars &lvs);

private:
  const UseDef *ud;

  map<IndexVar, pair<Var,ForDomain>> vertexLoopVars;
  map<Var, pair<Var,ForDomain>> edgeLoopVars;

  void visit(const SIGVertex *v) {
    static int i=0;  // hack workaround for esprings
    std::string name= (i==0) ? v->iv.getName() : v->iv.getName()+to_string(i++);

    Var lvar(v->iv.getName() + to_string(i++), Int);

    // The vertex is unconstrained and loops over it's whole domain.
    ForDomain ldom = v->iv.getDomain().getIndexSets()[0];

    vertexLoopVars[v->iv] = pair<Var,ForDomain>(lvar,ldom);
  }

  void visit(const SIGEdge *e) {
    std::string varName = "e";
    for (SIGVertex *nbr : e->endpoints) {
      varName += nbr->iv.getName();
    }
    Var lvar(varName, Int);

    VarDef varDef = ud->getDef(e->tensor);
    iassert(varDef.getKind() == VarDef::Map);

    const Map *mapStmt = to<Map>(varDef.getStmt());
    ForDomain ldom = ForDomain(mapStmt->target);

    edgeLoopVars[e->tensor] = pair<Var,ForDomain>(lvar,ldom);
  }
};

inline std::ostream &operator<<(std::ostream &os, const LoopVars &lvs) {
  for (auto &vlv : lvs.vertexLoopVars) {
    os << vlv.second.first << ",";
  }
  for (auto &elv : lvs.edgeLoopVars) {
    os << elv.second.first << ",";
  }
  return os;
}


/// Specializes index expressions to compute one value/block at the location
/// specified by the given loop variables
class RemoveIndexExprs : public IRRewriter {
public:
  RemoveIndexExprs(const LoopVars *lvs) : lvs(lvs) {}

private:
  const LoopVars *lvs;
  map<Var,Expr> varExprs;

  map<string,IndexVar> indexVars;

  Expr getVarExpr(const Var &var) {
    if (varExprs.find(var) == varExprs.end()) {
      varExprs[var] = VarExpr::make(var);
    }
    return varExprs.at(var);
  }

  void visit(const AssignStmt *op) {
    iassert(isa<IndexExpr>(op->value) && "Can only specialize IndexExpr stmts");
    const IndexExpr *indexExpr = to<IndexExpr>(op->value);

    Var var = op->var;

    Expr value = mutate(indexExpr);

    std::vector<IndexVar> indexVars = getFreeVars(value);
    std::vector<IndexVar> rindexVars = getReductionVars(value);

    // TODO: Simplify by emitting empty IndexedTensors for scalar
    //       expressions (in visit(IndexedTensor)). Then we can remove the test
    //       and always turn value into an IndexExpr
    if (indexVars.size() > 0 || rindexVars.size() > 0) {
      value = IndexExpr::make(indexVars, value);
    }

    if (indexExpr->resultVars.size() == 0) {
      stmt = AssignStmt::make(var, value);
    }
    else {
      Expr varExpr = getVarExpr(var);

      std::vector<Expr> indices;
      for (IndexVar const& iv : indexExpr->resultVars) {
        Expr varExpr = getVarExpr(lvs->getVar(iv).first);
        indices.push_back(varExpr);
      }
      stmt = TensorWrite::make(varExpr, indices, value);
    }
  }

  void visit(const FieldWrite *op) {
    iassert(isa<IndexExpr>(op->value)) << "Can only specialize IndexExpr stmts";
    const IndexExpr *indexExpr = to<IndexExpr>(op->value);

    Expr elementOrSet = mutate(op->elementOrSet);
    std::string fieldName = op->fieldName;
    Expr value = mutate(indexExpr);

    std::vector<IndexVar> indexVars = getFreeVars(value);
    std::vector<IndexVar> rindexVars = getReductionVars(value);

    // TODO: Simplify by emitting empty IndexedTensors for scalar
    //       expressions (in visit(IndexedTensor)). Then we can remove the test
    //       and always turn value into an IndexExpr
    if (indexVars.size() > 0 || rindexVars.size() > 0) {
      value = IndexExpr::make(indexVars, value);
    }

    if (indexExpr->resultVars.size() == 0) {
      stmt = FieldWrite::make(elementOrSet, fieldName, value);
    }
    else {
      std::vector<Expr> indices;
      for (IndexVar const& iv : indexExpr->resultVars) {
        Expr varExpr = getVarExpr(lvs->getVar(iv).first);
        indices.push_back(varExpr);
      }
      Expr field = FieldRead::make(elementOrSet, fieldName);
      stmt = TensorWrite::make(field, indices, value);
    }
  }

  void visit(const TensorWrite *op) {
    iassert(isa<IndexExpr>(op->value)) << "Can only specialize IndexExpr stmts";
    const IndexExpr *indexExpr = to<IndexExpr>(op->value);

    Expr value = mutate(op->value);

    if (isScalar(op->value.type())) {
      stmt = TensorWrite::make(op->tensor, op->indices, value);
    }
    else {
      std::vector<Expr> indices;
      for (IndexVar const& iv : indexExpr->resultVars) {
        Expr varExpr = getVarExpr(lvs->getVar(iv).first);
        indices.push_back(varExpr);
      }
      Expr tensor = TensorRead::make(op->tensor, op->indices);
      stmt = TensorWrite::make(tensor, indices, value);
    }
  }

  void visit(const IndexedTensor *op) {
    iassert(!isa<IndexExpr>(op->tensor))
        << "index expressions should have been lowered by now";

    if (op->indexVars.size() == 0) {
      expr = op->tensor;
    }
    else {
      std::vector<Expr> indices;
      for (const IndexVar &iv : op->indexVars) {
        Expr varExpr = getVarExpr(lvs->getVar(iv).first);
        indices.push_back(varExpr);
      }
      expr = TensorRead::make(op->tensor, indices);

      if (expr.type().toTensor()->order() > 0) {
        vector<IndexVar> ivs;
        for (auto &iv : op->indexVars) {
          if (indexVars.find(iv.getName()) == indexVars.end()) {
            vector<IndexSet> indexSets(iv.getDomain().getIndexSets().begin()+1,
                                       iv.getDomain().getIndexSets().end());
            IndexVar blockiv = IndexVar(iv.getName(), IndexDomain(indexSets),
                                        iv.getOperator());
            indexVars.insert(pair<string,IndexVar>(blockiv.getName(), blockiv));
          }
          ivs.push_back(indexVars.at(iv.getName()));
        }

        expr = IndexedTensor::make(expr, ivs);
      }
    }
  }

  void visit(const IndexExpr *op) {
    indexVars.clear();
    expr = mutate(op->value);
  }
};


class GetReductionTmpName : public IRVisitor {
public:
  string get(const TensorWrite *op) {
    op->tensor.accept(this);
    for (auto &index : op->indices) {
      index.accept(this);
    }
    return name;
  }

private:
  std::string name;

  void visit(const VarExpr *op) {
    IRVisitor::visit(op);
    name += op->var.getName();
  }
};

/// Turns tensor writes into compound assignments (e.g. +=, *=)
/// \todo Generalize to include Assignments, FieldWrite, TupleWrite
class MakeCompound : public IRRewriter {
public:
  enum CompoundOperator { Add };

  MakeCompound(CompoundOperator compoundOperator)
      : compoundOperator(compoundOperator) {}

  Stmt mutate(Stmt stmt) {
    return IRRewriter::mutate(stmt);
  }

private:
  CompoundOperator compoundOperator;
  Expr lhsExpr;

  Expr mutate(Expr e) {
    iassert(lhsExpr.defined());

    if (e.defined()) {
      if (!isScalar(e.type())) {
        e = IRRewriter::mutate(e);
      }
      else {
        switch (compoundOperator) {
          case Add:
            e = Add::make(lhsExpr, e);
            break;
        }
      }
    }
    else {
      e = Expr();
    }
    expr = Expr();
    stmt = Stmt();
    return e;
  }

  void visit(const TensorWrite *op) {
    lhsExpr = TensorRead::make(op->tensor, op->indices);

    vector<IndexVar> indexVars = getFreeVars(op->value);
    if (indexVars.size()) {
      lhsExpr = IndexedTensor::make(lhsExpr, indexVars);
    }

    Expr value = mutate(op->value);
    stmt = TensorWrite::make(op->tensor, op->indices, value);
  }
};

/// Rewrites rstmt to reduce it's computed value into a temporary reduction
/// variable using the rop ReductionOperation.
class ReduceOverVar : public IRRewriter {
public:
  ReduceOverVar(Stmt rstmt, ReductionOperator rop) : rstmt(rstmt), rop(rop) {}

  Var getTmpVar() {return tmpVar;}

  /// Retrieve a statement that writes the tmp variable to the original
  /// location of the rewritten statement.  If result is !defined then the
  /// reduction variable does not ned to be written back.
  Stmt getTmpWriteStmt() {return tmpWriteStmt;}

private:
  Stmt rstmt;
  ReductionOperator rop;

  Var tmpVar;
  Stmt tmpWriteStmt;

  void visit(const AssignStmt *op) {
    if (op == rstmt) {
      iassert(isScalar(op->value.type()))
          << "assignment non-scalars should have been lowered by now";
      switch (rop.getKind()) {
        case ReductionOperator::Sum: {
          Expr varExpr = VarExpr::make(op->var);
          tmpVar = op->var;
          stmt = AssignStmt::make(op->var, Add::make(varExpr, op->value));
          break;
        }
        case ReductionOperator::Undefined:
          ierror;
          break;
      }
    }
    else {
      stmt = op;
    }
  }

  void visit(const TensorWrite *op) {
    if (op == rstmt) {
      Expr tensor = op->tensor;
      std::vector<Expr> indices = op->indices;

      iassert(tensor.type().isTensor());
      switch (rop.getKind()) {
        case ReductionOperator::Sum: {
          ScalarType componentType = tensor.type().toTensor()->componentType;
          string tmpVarName = GetReductionTmpName().get(op);
          tmpVar = Var(tmpVarName, TensorType::make(componentType));
          stmt = AssignStmt::make(tmpVar, Add::make(tmpVar, op->value));
          break;
        }
        case ReductionOperator::Undefined:
          ierror;
          break;
      }

      tmpWriteStmt = TensorWrite::make(tensor, indices, VarExpr::make(tmpVar));
      if (isa<TensorRead>(tensor)) {
        tmpWriteStmt = MakeCompound(MakeCompound::Add).mutate(tmpWriteStmt);
      }
    }
  }
};


/// Get the variables that are used to index a tensor in an Expr as well as
/// the expression that indexes the tensors
class GetTensorRead : public IRVisitor {
public:
   const TensorRead *get(Var tensorVar, Stmt stmt) {
    this->tensorVar = tensorVar;
    stmt.accept(this);
    return tensorRead;
  }

private:
  Var tensorVar;
  const TensorRead *tensorRead;

  void visit(const TensorRead *op) {
    if (isa<VarExpr>(op->tensor)) {
      if (to<VarExpr>(op->tensor)->var == this->tensorVar) {
        tensorRead = op;
      }
    }
  }
};

// TODO: The if-based pattern matching in the visit rules is a total hack and
//       has to be rewritten.
class InlineMappedFunction : private IRRewriter {
public:
  InlineMappedFunction(Var lvar, Var resultActual, const Map *map,
                       Stmt computeStmt) {
    Func mapFunc = map->function;
    iassert(mapFunc.getArguments().size()==1||mapFunc.getArguments().size()==2)
        << "mapped functions must have exactly two arguments";

    this->loopVar = lvar;

    this->targetSet = map->target;
    this->neighborSet = map->neighbors;

    this->resultActual = resultActual;

    this->targetElement = mapFunc.getArguments()[0];
    this->neighborElements = mapFunc.getArguments()[1];

    this->results = set<Var>(mapFunc.getResults().begin(),
                             mapFunc.getResults().end());

    this->computeStmt = computeStmt;
  }

  Stmt rewrite(Stmt s) {
    s = mutate(s);
    return s;
  }

private:
  set<Var> results;

  Var targetElement;
  Var neighborElements;
  Var resultActual;

  Expr loopVar;
  Expr targetSet;
  Expr neighborSet;

  Stmt computeStmt;
  map<Expr,Expr> substitutions;

  /// Turn argument field reads into loads from the buffer corresponding to that
  /// field
  void visit(const FieldRead *op) {
    // TODO: Handle the case where the target var was reassigned
    //       tmp = s; ... = tmp.a;
    if (isa<VarExpr>(op->elementOrSet) &&
        to<VarExpr>(op->elementOrSet)->var == targetElement) {
      Expr setFieldRead = FieldRead::make(targetSet, op->fieldName);
      expr = TensorRead::make(setFieldRead, {loopVar});
    }
    else if(isa<TupleRead>(op->elementOrSet) &&
            isa<VarExpr>(to<TupleRead>(op->elementOrSet)->tuple) &&
            to<VarExpr>(to<TupleRead>(op->elementOrSet)->tuple)->var ==neighborElements) {
      Expr setFieldRead = FieldRead::make(neighborSet, op->fieldName);
      expr = setFieldRead;

      Expr index = mutate(op->elementOrSet);
      expr = TensorRead::make(setFieldRead, {index});
    }
    else {
      not_supported_yet;
    }
  }

  void visit(const TupleRead *op) {
    // TODO: Handle the case where the target var was reassigned
    //       tmp = p(0); ... = tmp.x;
    if (isa<VarExpr>(op->tuple) &&
        to<VarExpr>(op->tuple)->var == neighborElements) {
      const TupleType *tupleType = op->tuple.type().toTuple();
      int cardinality = tupleType->size;

      Expr endpoints = IndexRead::make(targetSet, "endpoints");
      Expr indexExpr = Add::make(Mul::make(loopVar, cardinality), op->index);
      expr = Load::make(endpoints, indexExpr);
    }
    else {
      IRRewriter::visit(op);
    }
  }

  void visit(const TensorWrite *op) {
    if (isa<VarExpr>(op->tensor)) {
      if (results.find(to<VarExpr>(op->tensor)->var) != results.end()) {
        Expr tensor = op->tensor;
        std::vector<Expr> indices;
        for (auto &index : op->indices) {
          indices.push_back(mutate(index));
        }
        Expr value = mutate(op->value);
        const TensorRead *tensorRead = GetTensorRead().get(resultActual,
                                                           computeStmt);
        iassert(tensorRead->indices.size() == indices.size());

        map<Expr,Expr> substitutions;
        substitutions[tensorRead] = value;
        for (size_t i=0; i<indices.size(); ++i) {
          substitutions[tensorRead->indices[i]] = indices[i];
        }

        stmt = substitute(substitutions, computeStmt);

        stmt = flattenIndexExpressions(stmt);
        bool hasReduction = containsReduction(stmt);
        if (!hasReduction) {
          stmt = MakeCompound(MakeCompound::Add).mutate(stmt);
        }
        return;
      }
    }
    IRRewriter::visit(op);
  }
};


class ReplaceRhsWithZero : public IRRewriter {
  void visit(const AssignStmt *op) {
    stmt = AssignStmt::make(op->var, 0.0);
  }

  void visit(const FieldWrite *op) {
    // TODO: Value can't be a float if the field is an integer field
    stmt = FieldWrite::make(op->elementOrSet, op->fieldName, 0.0);
  }

  void visit(const TensorWrite *op) {
    stmt = TensorWrite::make(op->tensor, op->indices, 0.0);
  }
};

/// Retrieves the index expression computed in a given Stmt.  If no index
/// expressions are computed in the Stmt an undefined Expr is returned.
class GetIndexExpr : private IRVisitor {
public:
  const IndexExpr *get(Stmt stmt) {
    stmt.accept(this);
    return indexExpr;
  }

private:
  const IndexExpr *indexExpr;

  void visit(const IndexExpr *op) {
    indexExpr = op;
  }
};

class LowerIndexExpressions : public IRRewriter {
public:
  LowerIndexExpressions(const UseDef *ud) : ud(ud) {}

private:
  const UseDef *ud;

  /// Lower the index statement.  Defined after the LoopBuilder due to a
  /// circular dependency.
  Stmt lower(Stmt stmt);

  void visit(const AssignStmt *op) {
    if (isa<IndexExpr>(op->value)) {
      stmt = lower(op);
    }
    else {
      IRRewriter::visit(op);
    }
  }

  void visit(const FieldWrite *op) {
    if (isa<IndexExpr>(op->value)) {
      stmt = lower(op);
    }
    else {
      IRRewriter::visit(op);
    }
  }

  void visit(const TensorWrite *op) {
    if (isa<IndexExpr>(op->value)) {
      stmt = lower(op);
    }
    else {
      IRRewriter::visit(op);
    }
  }

  void visit(const IndexExpr *op) {
    ierror << "IndexExprs must be assigned to var/field/tensor before lowering";
  }
};

class LoopBuilder : public SIGVisitor {
public:
  LoopBuilder(const UseDef *ud) : ud(ud) {}

  Stmt create(Stmt computeStmt) {
    const IndexExpr *indexExpr = GetIndexExpr().get(computeStmt);

    SIG sig = SIGBuilder(ud).create(indexExpr);
    loopVars = LoopVars(sig, ud);

    initToZeroStmt = ReplaceRhsWithZero().mutate(computeStmt);

    // Create the loop body from the IndexExpr computeStmt
    loopBody = RemoveIndexExprs(&loopVars).mutate(computeStmt);
    std::vector<const SIGEdge *> edges = sig.getEdges();

    if (edges.size() == 0) { //&& indexExpr->type.toTensor()->dimensions.size()) {
      loopBody = LowerIndexExpressions(ud).mutate(loopBody);
    }

    if (edges.size() > 1) {
      not_supported_yet;
    }

    std::map<Var,const Map*> vars2maps;
    for (auto &e : edges) {
      VarDef varDef = ud->getDef(e->tensor);
      iassert(varDef.getKind() == VarDef::Map);

      Var lvar = loopVars.getVar(e->tensor).first;
      const Map *map = to<Map>(varDef.getStmt());

      vars2maps[e->tensor] = map;

      Func func = map->function;

      // Inline the mapped function in the IndexExpr loop nests
      Stmt funcBody = func.getBody();

      loopBody = InlineMappedFunction(lvar, e->tensor, map,
                                      loopBody).rewrite(funcBody);

      // TODO: We should knock out redundant subexpressions in the loopBody
      //       before lowering the index expressions there
      loopBody = flattenIndexExpressions(loopBody);

      UseDef fud(func);
      loopBody = LowerIndexExpressions(&fud).mutate(loopBody);
    }

    stmt = loopBody;
    apply(sig);
    Stmt result = stmt;
    stmt = Stmt();

    return result;
  }

private:
  const UseDef *ud;
  LoopVars loopVars;
  Stmt initToZeroStmt;
  Stmt loopBody;

  Stmt stmt;

  std::vector<const SIGEdge *> edges;

  void visit(const SIGVertex *v) {
    pair<Var,ForDomain> loopVar = loopVars.getVar(v->iv);
    Var lvar = loopVar.first;
    ForDomain ldom = loopVar.second;

    if (v->iv.isFreeVar()) {
      stmt = For::make(lvar, ldom, stmt);
    }
    else {
      ReduceOverVar rov(loopBody, v->iv.getOperator());
      Stmt loopBody = rov.mutate(stmt);

      Var tmpVar = rov.getTmpVar();

      iassert(tmpVar.defined());

      Stmt alloc = AssignStmt::make(tmpVar,Literal::make(tmpVar.getType(),{0}));
      Stmt loop = For::make(lvar, ldom, loopBody);

      Stmt tmpWriteStmt = rov.getTmpWriteStmt();
      if (tmpWriteStmt.defined()) {
        stmt = Block::make(alloc, Block::make(loop, tmpWriteStmt));
      }
      else {
        stmt = Block::make(alloc, loop);
      }
    }

    SIGVisitor::visit(v);
  }

  void visit(const SIGEdge *e) {
    edges.push_back(e);

    pair<Var,ForDomain> loopVar = loopVars.getVar(e->tensor);

    Var lvar = loopVar.first;
    ForDomain ldom = loopVar.second;

    VarDef varDef = ud->getDef(e->tensor);
    iassert(varDef.getKind() == VarDef::Map);

    Stmt loop = For::make(lvar, ldom, loopBody);
    stmt = Block::make(initToZeroStmt, loop);

    for (auto &v : e->endpoints) {
      visitedVertices.insert(v);
    }
  }
};

bool isNestedDotProductAssign(Stmt stmt) {
  if (!isa<AssignStmt>(stmt)) {
    return false;
  }

  const AssignStmt *assign = to<AssignStmt>(stmt);
  Expr value = assign->value;

  if (!isa<IndexExpr>(value)) {
    return false;
  }

  const IndexExpr *iexpr = to<IndexExpr>(value);
  if (!isScalar(iexpr->type)) {
    return false;
  }

  if (!isa<Mul>(iexpr->value)) {
    return false;
  }

  const Mul *mul = to<Mul>(iexpr->value);
  if (!mul->a.type().isTensor() || !mul->b.type().isTensor()) {
    return false;
  }

  if (!isa<IndexedTensor>(mul->a) || !isa<IndexedTensor>(mul->b)) {
    return false;
  }

  const IndexedTensor *ait = to<IndexedTensor>(mul->a);
  const IndexedTensor *bit = to<IndexedTensor>(mul->b);

  const TensorType *atype = ait->tensor.type().toTensor();
  const TensorType *btype = bit->tensor.type().toTensor();

  if (atype->order() != 1 || btype->order() != 1) {
    return false;
  }

  if (atype->dimensions[0].getIndexSets().size() != 2 ||
      btype->dimensions[0].getIndexSets().size() != 2) {
    return false;
  }

  return true;
}



class LowerNestedDot : public IRRewriter {
public:
  Var lv1;
  Var lv2;
  vector<Expr> operands;

private:
  void visit(const AssignStmt *op) {
    if (isa<IndexExpr>(op->value)) {
      Stmt stmts;
      Var rvar = op->var;

      Stmt alloc = AssignStmt::make(rvar,Literal::make(rvar.getType(),{0}));

      lv1 = Var("lv1", Int);
      lv2 = Var("lv2", Int);

      Expr value = mutate(op->value);

      value = Add::make(VarExpr::make(rvar), value);

      Stmt body = AssignStmt::make(rvar, value);

      IndexDomain dims = operands[0].type().toTensor()->dimensions[0];
      auto iss = dims.getIndexSets();
      iassert(iss.size() == 2);

      Stmt loop = For::make(lv2, ForDomain(iss[1]), body);
      loop = For::make(lv1, ForDomain(iss[0]), loop);

      stmt = stmts = Block::make(alloc, loop);
    }
    else {
      IRRewriter::visit(op);
    }
  }

  void visit(const IndexExpr *op) {
    expr = mutate(op->value);
  }

  void visit(const IndexedTensor *op) {
    Expr tensor = op->tensor;
    operands.push_back(tensor);

    Expr tread = TensorRead::make(tensor, {VarExpr::make(lv1)});
    tread = TensorRead::make(tread, {VarExpr::make(lv2)});

    expr = tread;
  }
};

Stmt LowerIndexExpressions::lower(Stmt stmt) {
  if (isNestedDotProductAssign(stmt)) {
    Stmt tmp = LowerNestedDot().mutate(stmt);
    return tmp;
  }
  else {
    return LoopBuilder(ud).create(stmt);
  }
}

Stmt lowerIndexExpressions(Stmt stmt, const UseDef &ud) {
  return LowerIndexExpressions(&ud).mutate(stmt);
}

Func lowerIndexExpressions(Func func, const TensorStorages &tensorStorages) {
  UseDef ud(func);
  Stmt body = lowerIndexExpressions(func.getBody(), ud);
  return Func(func, body);
}


}}