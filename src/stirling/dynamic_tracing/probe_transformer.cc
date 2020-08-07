#include "src/stirling/dynamic_tracing/probe_transformer.h"

#include <map>
#include <string>
#include <utility>

#include "src/stirling/dynamic_tracing/goid.h"
#include "src/stirling/dynamic_tracing/ir/sharedpb/shared.pb.h"
#include "src/stirling/dynamic_tracing/types.h"

namespace pl {
namespace stirling {
namespace dynamic_tracing {

// TODO(yzhao): Add using declaration of Language, such that it needn't repeat.

constexpr char kStartKTimeNSVarName[] = "start_ktime_ns";

void CreateMap(const ir::logical::Probe& input_probe, ir::logical::TracepointSpec* out) {
  if (input_probe.args().empty()) {
    return;
  }
  auto* stash_map = out->add_maps();
  stash_map->set_name(input_probe.name() + "_argstash");
}

ir::shared::BPFHelper GetLanguageThreadID(const ir::shared::Language& language) {
  switch (language) {
    case ir::shared::GOLANG:
      return ir::shared::BPFHelper::GOID;
    default:
      // Default (e.g. C/C++): assume no special runtime.
      return ir::shared::BPFHelper::TGID_PID;
  }
}

namespace {

bool IsFunctionLatecySpecified(const ir::logical::Probe& probe) {
  return probe.function_latency_oneof_case() ==
         ir::logical::Probe::FunctionLatencyOneofCase::kFunctionLatency;
}

}  // namespace

void CreateEntryProbe(const ir::shared::Language& language, const ir::logical::Probe& input_probe,
                      ir::logical::TracepointSpec* out) {
  auto* entry_probe = out->add_probes();
  entry_probe->mutable_trace_point()->CopyFrom(input_probe.trace_point());
  entry_probe->mutable_trace_point()->set_type(ir::shared::TracePoint::ENTRY);
  entry_probe->set_name(input_probe.name() + "_entry");

  // Access arguments.
  for (const auto& in_arg : input_probe.args()) {
    auto* out_arg = entry_probe->add_args();
    out_arg->CopyFrom(in_arg);
  }

  // Generate argument stash.
  // For now, always stash all arguments.
  if (input_probe.args_size() > 0 || IsFunctionLatecySpecified(input_probe)) {
    std::string map_name = input_probe.name() + "_argstash";

    auto* stash_action = entry_probe->add_map_stash_actions();
    stash_action->set_map_name(map_name);
    stash_action->set_key(GetLanguageThreadID(language));

    for (const auto& in_arg : input_probe.args()) {
      stash_action->add_value_variable_name(in_arg.id());
    }

    if (IsFunctionLatecySpecified(input_probe)) {
      // Insert the entry time into map, which will be unstashed in the return probe.
      stash_action->add_value_variable_name("time_");
    }
  }
}

Status CheckOutputAction(const std::map<std::string_view, ir::logical::Output*>& outputs,
                         const ir::logical::OutputAction& output_action) {
  auto iter = outputs.find(output_action.output_name());
  if (iter == outputs.end()) {
    return error::Internal("Reference to unknown output $0", output_action.output_name());
  }
  const ir::logical::Output& output = *iter->second;

  if (output_action.variable_name_size() != output.fields_size()) {
    return error::Internal("Output action size $0 does not match Output definition size $1",
                           output_action.variable_name_size(), output.fields_size());
  }

  return Status::OK();
}

Status CreateReturnProbe(const ir::shared::Language& language,
                         const ir::logical::Probe& input_probe,
                         const std::map<std::string_view, ir::logical::Output*>& outputs,
                         ir::logical::TracepointSpec* out) {
  auto* return_probe = out->add_probes();
  return_probe->set_name(input_probe.name() + "_return");
  return_probe->mutable_trace_point()->CopyFrom(input_probe.trace_point());
  return_probe->mutable_trace_point()->set_type(ir::shared::TracePoint::RETURN);

  if (input_probe.args_size() > 0 || IsFunctionLatecySpecified(input_probe)) {
    std::string map_name = input_probe.name() + "_argstash";

    auto* map_val = return_probe->add_map_vals();
    map_val->set_map_name(map_name);
    map_val->set_key(GetLanguageThreadID(language));

    for (const auto& in_arg : input_probe.args()) {
      map_val->add_value_ids(in_arg.id());
    }

    // The order must be consistent with the MapStashAction.
    if (IsFunctionLatecySpecified(input_probe)) {
      // This refers to the value stashed in the entry probe.
      //
      // TODO(yzhao): We should add Variable into intermediate IR, and let the logical ->
      // intermediate translation produces the special variables.
      map_val->add_value_ids(kStartKTimeNSVarName);
    }

    auto* map_delete_action = return_probe->add_map_delete_actions();
    map_delete_action->set_map_name(map_name);
    map_delete_action->set_key(GetLanguageThreadID(language));
  }

  // Generate return values.
  for (const auto& in_ret_val : input_probe.ret_vals()) {
    auto* out_ret_val = return_probe->add_ret_vals();
    out_ret_val->CopyFrom(in_ret_val);
  }

  if (IsFunctionLatecySpecified(input_probe)) {
    // Function latency is left for coge_gen.cc to process.
    return_probe->mutable_function_latency()->CopyFrom(input_probe.function_latency());
  }

  // Generate output action.
  for (const auto& in_output_action : input_probe.output_actions()) {
    auto* output_action = return_probe->add_output_actions();
    output_action->CopyFrom(in_output_action);
    PL_RETURN_IF_ERROR(CheckOutputAction(outputs, *output_action));
  }

  for (const auto& printk : input_probe.printks()) {
    return_probe->add_printks()->CopyFrom(printk);
  }

  return Status::OK();
}

Status TransformLogicalProbe(const ir::shared::Language& language,
                             const ir::logical::Probe& input_probe,
                             const std::map<std::string_view, ir::logical::Output*>& outputs,
                             ir::logical::TracepointSpec* out) {
  // A logical probe is allowed to implicitly access arguments and return values.
  // Here we expand this out to be explicit. We break the logical probe into:
  // 1) An entry probe - to grab any potential arguments.
  // 2) A return probe - to grab any potential return values.
  // 3) A map - to stash the arguments and transfer them to the return probe.
  // TODO(oazizi): An optimization could be to determine whether both entry and return probes
  //               are required. When not required, one probe and the stash map can be avoided.
  out->set_language(language);
  CreateMap(input_probe, out);
  CreateEntryProbe(language, input_probe, out);
  PL_RETURN_IF_ERROR(CreateReturnProbe(language, input_probe, outputs, out));

  return Status::OK();
}

StatusOr<ir::logical::TracepointDeployment> TransformLogicalProgram(
    const ir::logical::TracepointDeployment& input_program) {
  ir::logical::TracepointDeployment out;

  std::map<std::string_view, ir::logical::Output*> outputs;

  // Copy the binary path.
  out.mutable_deployment_spec()->CopyFrom(input_program.deployment_spec());

  // For each input Tracepoint, generate probes that needed for producing data between entry and
  // return of the target function.
  for (const auto& input_tracepoint : input_program.tracepoints()) {
    auto* out_tracepoint = out.add_tracepoints();

    out_tracepoint->set_output_name(input_tracepoint.output_name());

    const auto& input_tracepoint_spec = input_tracepoint.program();
    auto* out_tracepoint_spec = out_tracepoint->mutable_program();

    out_tracepoint_spec->set_language(input_tracepoint.program().language());

    // Copy all explicitly declared output buffers.
    for (const auto& o : input_tracepoint_spec.outputs()) {
      auto* output = out_tracepoint_spec->add_outputs();
      output->CopyFrom(o);
      outputs[output->name()] = output;
    }

    // Copy all explicitly declared maps.
    for (const auto& m : input_tracepoint_spec.maps()) {
      auto* map = out_tracepoint_spec->add_maps();
      map->CopyFrom(m);
    }

    if (!input_tracepoint_spec.probes().empty()) {
      if (input_tracepoint_spec.language() == ir::shared::GOLANG) {
        out_tracepoint_spec->add_maps()->CopyFrom(GenGOIDMap());
        out_tracepoint_spec->add_probes()->CopyFrom(GenGOIDProbe());
      }
    }

    for (const auto& p : input_tracepoint_spec.probes()) {
      // TODO(yzhao): Turn this into a DCHECK() with the same condition, and remove else branch.
      if (p.trace_point().type() == ir::shared::TracePoint::LOGICAL) {
        PL_RETURN_IF_ERROR(TransformLogicalProbe(input_tracepoint_spec.language(), p, outputs,
                                                 out_tracepoint_spec));
      } else {
        auto* probe = out_tracepoint_spec->add_probes();
        probe->CopyFrom(p);
      }
    }
  }

  return out;
}

}  // namespace dynamic_tracing
}  // namespace stirling
}  // namespace pl
