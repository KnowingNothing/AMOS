#include <cmath>

#include "interpreter.h"
#include "auto_schedule.h"


namespace tvm {


namespace tg {

std::pair<te::Schedule, Array<te::Tensor> >
empty_schedule (TIRGraph subgraph) {
  te::Schedule sch = te::create_schedule(subgraph->root_ops);
  // Array<te::Tensor> tensors;
  // for (auto t : subgraph->inputs) {
  //   tensors.push_back(t);
  // }
  // for (auto t : subgraph->labels) {
  //   tensors.push_back(t);
  // }
  // for (auto t : subgraph->outputs) {
  //   tensors.push_back(t);
  // }
  // for (auto t : subgraph->weights) {
  //   tensors.push_back(t);
  // }
  // if (subgraph->loss.defined()) {
  //   tensors.push_back(subgraph->loss);
  // }
  // for (auto t : subgraph->gradients) {
  //   tensors.push_back(t);
  // }
  // if (subgraph->lr.defined()) {
  //   tensors.push_back(subgraph->lr);
  // }
  // for (auto t : subgraph->updates) {
  //   tensors.push_back(t);
  // }

  return std::make_pair(sch, subgraph->tensors);
}


double calculate_possibility(double x, double best, double upper=0.7) {
  return std::exp(x/best - 1.0) * upper;
}


std::vector<double> AutoScheduler::judge_schedule(
  Array<te::Schedule> schedules, Array<te::Tensor> tensors, Target target, std::string policy, double gflop) {
  const auto* f = runtime::Registry::Get("tg.autoschedule.query_cost_model");
  ASSERT(f != nullptr) << "Can't find tg.autoschedule.query_cost_model";
  std::vector<double> ret;
  Array<FloatImm> tmp = (*f)(schedules, tensors, target, policy);
  for (auto v : tmp) {
    if (v->value <= 0) {
      ret.push_back(0.0);
    } else {
      ret.push_back(gflop / (v->value / 1e3));
    }
  }

  return ret;
}


// auto_schedule for one subgraph
void AutoScheduler::auto_schedule(
    TIRGraph subgraph,
    AutoScheduleContext &context,
    ScheduleResult &results) {
  /* the empty schedule */
  te::Schedule sch;
  Array<te::Tensor> tensors;
  std::tie(sch, tensors) = empty_schedule(subgraph);

  /* the schedule logic
   * a schedule is two-level: skeleton + paramter
   * when the topk cache is empty, all random enumerated
   * when the topk cache is not empty, choose skeleton from cache
   * with possibility 'p', and random enumerate paramter
   * according to the chosen skeleton.
   * Otherwise, still all random.
   */
  std::vector<EvaluatedScheduleResult> reverse_sort;
  std::vector<double> p;
  while (!context->topk_schedules.empty()) {
    reverse_sort.push_back(context->topk_schedules.top());
    context->topk_schedules.pop();
  }

  // return these topks, otherwise, they will be lost
  for (auto ele : reverse_sort) {
    context->topk_schedules.push(ele);
  }

  int num_candidates = (int)(reverse_sort.size());
  // calculate possbilities
  for (auto e : reverse_sort) {
    p.push_back(
      calculate_possibility(
        e->evaluation, reverse_sort[num_candidates - 1]->evaluation, 1.0));
  }

  std::cout << "Moniter schedule context\n" << std::flush;
  if (num_candidates > 0)
    std::cout << "Best: [" << reverse_sort[num_candidates-1]->evaluation << "]\n" << std::flush;
  else
    std::cout << "Best: [inf]\n" << std::flush;
  for (int i = 0; i < num_candidates; ++i) {
    std::cout << "(" << i << ")" << reverse_sort[i]->evaluation << "[" << p[i] << "] " << std::flush;
  }
  std::cout << "\n" << std::flush;

  // prepare new candidates
  std::vector<MultiScheduleEntity> new_candidates;
  int must_new = context->new_trial;
  while ((int)new_candidates.size() < context->new_trial) {
    print(4, log_out) << "schedule not full...\n";
    // choose a seed
    bool use_seed = false;
    EvaluatedScheduleResult seed;
    if (randdouble() < 0.8 && context->counts > warm_up_trials) {
      for (int k = 0; k < num_candidates; ++k) {
        int j = randint(k, num_candidates);
        if (randdouble() <= p[j]) {
          use_seed = true;
          seed = reverse_sort[j];
          std::cout << "choose " << j << "\n" << std::flush;
          break;
        }
      }
    }
    // choose new one
    MultiScheduleEntity new_one;
    if (use_seed) {
      std::cout << "Seed:\n" << std::flush;
      new_one = context->spaces.choose_one(seed->schedule_result->schedule_entities);
    } else {
      // pure random
      new_one = context->spaces.choose_one();
      std::cout << "Random:\n" << std::flush;
    }
    // if must_new, then must be new candidate never met before
    if (must_new > 0) {
      if ((context->known_schedules.find(new_one) == context->known_schedules.end())
          && (context->knowing_schedules.find(new_one) == context->knowing_schedules.end())) {
        new_candidates.push_back(new_one);
      } else {
        std::cout << "Repeat!\n" << std::flush;
      }
    } else {
      new_candidates.push_back(new_one);
    }
    // if (context->knowing_schedules.size() > 2000U) {
    //   context->known_schedules.clear();
    //   context->known_schedules = context->knowing_schedules;
    //   context->knowing_schedules.clear();
    // }
    must_new = -1;  // the next round, just relaxed
  }
  // choose from new candidates
  double best_value = -1;
  int best_ind = -1;
  int num_new_candidates = (int)new_candidates.size();
  Array<te::Schedule> tmp_schedules;
  for (int i = 0; i < num_new_candidates; ++i) {
    te::Schedule tmp_sch = te::create_schedule(subgraph->root_ops);
    interpret(tmp_sch, tensors, subgraph, context->target, new_candidates[i]);
    tmp_schedules.push_back(tmp_sch);
  }

  double gflop = get_gflop(subgraph);
  std::vector<double> tmp_judges = judge_schedule(tmp_schedules, tensors, context->target, context->policy, gflop);
  for (int i = 0; i < num_new_candidates; ++i) {
    // if (context->policy == "profile") {
    //   context.add_feedback(ScheduleResult(tmp_schedules[i], tensors, new_candidates[i]), tmp_judges[i]);
    // }
    if (tmp_judges[i] > best_value) {
      best_ind = i;
      best_value = tmp_judges[i];
    }
  }

  if (report_profile) {
    log_out << "check judge values:\n";
    for (auto v : tmp_judges) {
      log_out << v << " ";
    }
    log_out << "\n";
  }

  MultiScheduleEntity result_entity = new_candidates[best_ind];
  print(4, log_out) << "Check subgraph:\n" << subgraph->tag << "\n";
  print(4, log_out) << "Check schedule entity:\n" << result_entity.to_string() << "\n";
  interpret(sch, tensors, subgraph, context->target, result_entity);
  results = ScheduleResult(sch, tensors, result_entity);
  context->counts += 1;
  context->knowing_schedules.insert(result_entity);
}


void AutoScheduleContext::add_feedback(ScheduleResult schedule_result, double evaluation) {
  auto self = (*this);
  if (evaluation > 0.0) {
    EvaluatedScheduleResult evaluated = EvaluatedScheduleResult(schedule_result, evaluation);
    if ((int)self->topk_schedules.size() < self->topk) {
      self->topk_schedules.push(evaluated);
    } else {
      if (evaluated < self->topk_schedules.top()) {
        return;
      } else {
        self->topk_schedules.pop();
        self->topk_schedules.push(evaluated);
      }
    }
  }

  self->known_schedules.insert(schedule_result->schedule_entities);
  self->knowing_schedules.erase(schedule_result->schedule_entities);
  if (self->known_schedules.size() > 2000U) {
    int count = 0;
    std::vector<MultiScheduleEntity> to_delete;
    for (auto val : self->known_schedules) {
      to_delete.push_back(val);
      if (count > 1000)
        break;
      count += 1;
    }
    for (auto val : to_delete) {
      self->known_schedules.erase(val);
    }
  }
}


ScheduleResult AutoScheduler::schedule_func(IntKey key, TIRGraph subgraph, Target target) {
  if (contexts.find(key) == contexts.end()) {
    contexts[key] = AutoScheduleContext(key, subgraph, target, topk, new_trial, policy);
  }

  AutoScheduleContext context = contexts[key];
  ScheduleResult results;
  auto_schedule(subgraph, context, results);
  
  return results;
}


ScheduleResult AutoScheduler::schedule_with_entity(
  TIRGraph subgraph, Target target, MultiScheduleEntity entity) {
  // if (contexts.find(key) == contexts.end()) {
  //   contexts[key] = AutoScheduleContext(key, subgraph, target, topk, new_trial, policy);
  // }

  te::Schedule sch;
  Array<te::Tensor> tensors;
  std::tie(sch, tensors) = empty_schedule(subgraph);
  interpret(sch, tensors, subgraph, target, entity);
  return ScheduleResult(sch, tensors, entity);
}


std::shared_future<ScheduleResult> AutoScheduler::schedule_for(
  IntKey key, TIRGraph subgraph, Target target, int priority) {
  
  if (priority == 0) {
    return thread_pool->push_back(
      [this] (IntKey k, TIRGraph g, Target t) {
        return this->schedule_func(k, g, t);
        }, key, subgraph, target);
  } else if (priority == 1) {
    return thread_pool->push_front(
      [this] (IntKey k, TIRGraph g, Target t) {
        return this->schedule_func(k, g, t);
        }, key, subgraph, target);
  } else {
    ERROR << "Unsupported schedule priority: " << priority << "\n";
    throw;
  }
}


void AutoScheduler::feedback_for(IntKey key, TIRGraph subgraph, Target target, ScheduleResult schedule_result, double evaluation) {
  const auto* f = runtime::Registry::Get("tg.autoschedule.store_feedback");
  ASSERT(f != nullptr) << "Can't find tg.autoschedule.store_feedback";
  if (contexts.find(key) == contexts.end()) {
    contexts[key] = AutoScheduleContext(key, subgraph, target, topk, new_trial, policy);
  }
  contexts[key].add_feedback(schedule_result, evaluation);
  Array<Feature> feature = get_feature(schedule_result->schedule, schedule_result->tensors, contexts[key]->target);
  std::ostringstream oss;
  double gflop = get_gflop(subgraph);

  oss << "{ ";
  oss << "\"gflop\": ";
  oss << gflop << ", ";
  oss << "\"loop_nests\": ";
  oss << "[";
  for (int i = 0; i < (int)feature.size(); ++i) {
    if (i != 0)
      oss << ", ";
    oss << std::pow(2, feature[i]->features[15]->value);
  }
  oss << "], ";
  oss << "\"features\": ";
  oss << "[";
  for (int i = 0; i < (int)feature.size(); ++i) {
    if (i != 0)
      oss << ", ";
    oss << feature[i];
  }
  oss << "], ";
  // oss << "\"schedules\": ";
  // oss << "\"" << schedule_result->schedule_entities.to_string() << "\", ";
  oss << "\"evaluation\": ";
  oss << evaluation;
  oss << " }\n";
  profile_log << oss.str();
  
  if (evaluation > 0)
    (*f)(oss.str());
}


void AutoScheduler::clear_schedule_cache_for(IntKey key) {
  if (contexts.find(key) != contexts.end()) {
    auto context = contexts[key];
    context->knowing_schedules.clear();
  }
}


}  // namespace tg


}  // namespace tvm