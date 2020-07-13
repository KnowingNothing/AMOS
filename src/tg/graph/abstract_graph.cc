#include "abstract_graph.h"


namespace tvm {

namespace tg {

PrimExpr ExprReMapper::VisitExpr_(const VarNode* op) {
  if (var_map.find(op) != var_map.end()) {
    return var_map[op];
  }
  Var ret = Var(get_new_var_name(), op->dtype);
  var_map[op] = ret;
  return ret;
}

PrimExpr ExprReMapper::VisitExpr_(const SizeVarNode* op) {
  if (size_var_map.find(op) != size_var_map.end()) {
    return size_var_map[op];
  }
  SizeVar ret = SizeVar(get_new_var_name(), op->dtype);
  size_var_map[op] = ret;
  return ret;
}


PrimExpr ExprReMapper::VisitExpr_(const CallNode* op) {
  Array<PrimExpr> new_args;
  for (auto v : op->args) {
    new_args.push_back(VisitExpr(v));
  }
  
  if (op->call_type == CallNode::CallType::Halide) {
    if (call_map.find(op->func) != call_map.end()) {
      return CallNode::make(
        op->dtype,
        call_map[op->func],
        new_args,
        op->call_type,
        op->func,
        op->value_index
      );
    } else {
      std::string new_name = get_new_tensor_name();
      call_map[op->func] = new_name;
      return CallNode::make(
        op->dtype,
        new_name,
        new_args,
        op->call_type,
        op->func,
        op->value_index
      );
    }
  } else {
    return CallNode::make(
      op->dtype,
      op->name,
      new_args,
      op->call_type,
      op->func,
      op->value_index
    );
  }
}


PrimExpr ExprReMapper::VisitExpr_(const ReduceNode* op) {
  CommReducer reducer;
  Array<Var> lhs;
  Array<Var> rhs;
  Array<PrimExpr> results;
  Array<PrimExpr> identities;
  for (Var l : op->combiner->lhs) {
    if (var_map.find(l.get()) != var_map.end()) {
      lhs.push_back(var_map[l.get()]);
    } else {
      VisitExpr(l);
      lhs.push_back(var_map[l.get()]);
    }
  }
  for (auto r : op->combiner->rhs) {
    if (var_map.find(r.get()) != var_map.end()) {
      rhs.push_back(var_map[r.get()]);
    } else {
      VisitExpr(r);
      rhs.push_back(var_map[r.get()]);
    }
  }
  for (auto r : op->combiner->result) {
    results.push_back(VisitExpr(r));
  }
  for (auto i : op->combiner->identity_element) {
    identities.push_back(VisitExpr(i));
  }
  reducer = CommReducerNode::make(lhs, rhs, results, identities);

  
  Array<PrimExpr> source;
  for (auto s : op->source) {
    source.push_back(VisitExpr(s));
  }

  Array<IterVar> axis;
  for (auto iv : op->axis) {
    VisitExpr(iv->var);
    axis.push_back(
      IterVarNode::make(iv->dom, var_map[iv->var.get()], iv->iter_type, iv->thread_tag));
  }

  PrimExpr condition = this->VisitExpr(op->condition);

  return ReduceNode::make(
    reducer, source, axis, condition, op->value_index);
}


std::string generate_tag_from_body(Array<PrimExpr>& shape, Array<PrimExpr>& body) {
  std::ostringstream oss;
  oss.str("");
  if (body.size() == 0U) {
    return oss.str();
  }

  const ReduceNode* as_reduce = body[0].as<ReduceNode>();

  if (as_reduce != nullptr) {
    CHECK(body.size() == 1U) << "Only support reduce with one body.";
    ExprReMapper remapper;
    PrimExpr new_reduce = remapper(body[0]);
    const ReduceNode* as_reduce = new_reduce.as<ReduceNode>();
    CHECK(as_reduce != nullptr);

    oss << "R[";
    bool add_colon = false;
    for (auto s : shape) {
      if (add_colon) {
        oss << ", ";
      } else {
        add_colon = true;
      }
      oss << s;
    }
    oss << "] [";
    add_colon = false;
    for (auto iv : as_reduce->axis) {
      if (add_colon) {
        oss << ", ";
      } else {
        add_colon = true;
      }
      oss << iv->dom->extent;
    }
    oss << "] { ";
    oss << as_reduce->combiner;
    oss << " } { ";
    for (size_t i = 0; i < as_reduce->source.size(); ++i) {
      if (i != 0) {
        oss << "; ";
      }
      oss << as_reduce->source[i];
    }
    oss << " }";
  } else {
    // not reduce
    oss << "S[";
    bool add_colon = false;
    for (auto s : shape) {
      if (add_colon) {
        oss << ", ";
      } else {
        add_colon = true;
      }
      oss << s;
    }
    oss << "] [ ] { } { ";
    bool add_semicolon = false;
    for (auto b : body) {
      CHECK(b.as<ReduceNode>() == nullptr) << "Should only contain non-reduce expr.";
      ExprReMapper remapper;
      PrimExpr new_b = remapper(b);
      if (add_semicolon) {
        oss << "; ";
      } else {
        add_semicolon = true;
      }
      oss << new_b;
    }
    oss << " }";
  }

  return oss.str();
}


TVM_REGISTER_GLOBAL("tg.generate_tag_from_body")
.set_body_typed([](Array<PrimExpr> shape, Array<PrimExpr> body) {
  return generate_tag_from_body(shape, body);
});


}  // namespace tg

}  // namespace tvm