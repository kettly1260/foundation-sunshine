#!/usr/bin/env python3
"""Selective port of LizardByte/Sunshine PR #5818 (per-stream input order).

Applied only to Windows ARM64 experimental builds, after the Foundation ARM64
patch scripts. Leaves Foundation pen-roll validation and input activity boost
untouched. If the fork diverges, fail rather than change dispatch semantics.
"""
from pathlib import Path

source = Path(__file__).resolve().parent.parent / "src" / "input.cpp"
content = source.read_text(encoding="utf-8")


def replace_once(old: str, new: str, why: str) -> None:
    global content
    matches = content.count(old)
    if matches != 1:
        raise RuntimeError(
            f"LizardByte input order port {why}: expected one anchor, found {matches}: {old[:100]!r}"
        )
    content = content.replace(old, new, 1)


if "input_task_scheduled" in content:
    raise RuntimeError("LizardByte input order port was already applied")

replace_once(
    "    std::list<std::vector<uint8_t>> input_queue;\n"
    "    std::mutex input_queue_lock;\n",
    "    std::list<std::vector<uint8_t>> input_queue;\n"
    "    std::mutex input_queue_lock;\n"
    "    // Exactly one input task owns this stream's queue; guarded by input_queue_lock.\n"
    "    bool input_task_scheduled = false;\n",
    "stream ownership flag",
)

replace_once(
    "  /**\n"
    "   * @brief Called on a thread pool thread to process an input message.\n"
    "   * @param input The input context pointer.\n"
    "   */\n"
    "  void\n"
    "  passthrough_next_message(std::shared_ptr<input_t> input) {\n",
    "  // A single worker owns packet dispatch for a stream, preserving mouse,\n"
    "  // keyboard and pen event order even with multiple task-pool threads.\n"
    "  void passthrough_queued_messages(std::shared_ptr<input_t> input);\n"
    "\n"
    "  void schedule_input_packet_task(const std::shared_ptr<input_t> &input) {\n"
    "    task_pool.push(passthrough_queued_messages, input);\n"
    "  }\n"
    "\n"
    "  // Process one queued packet; return false only when the queue is empty.\n"
    "  bool passthrough_next_message(std::shared_ptr<input_t> &input) {\n",
    "dispatch refactor",
)

replace_once(
    "      if (input->input_queue.empty()) {\n"
    "        return;\n"
    "      }\n",
    "      if (input->input_queue.empty()) {\n"
    "        input->input_task_scheduled = false;\n"
    "        return false;\n"
    "      }\n",
    "clear ownership only for drained queue",
)

replace_once(
    "      if (!input->activity_tracker.evaluate(payload, entry.size()).has_value()) {\n"
    "        return;\n"
    "      }\n",
    "      if (!input->activity_tracker.evaluate(payload, entry.size()).has_value()) {\n"
    "        // A rejected packet was dequeued. Continue dispatching remaining\n"
    "        // messages under the same stream ownership.\n"
    "        return true;\n"
    "      }\n",
    "invalid packet must not strand queued valid packets",
)

replace_once(
    "    if (should_raise_input_activity) {\n"
    "      input->input_activity_event->raise(std::chrono::steady_clock::now());\n"
    "    }\n"
    "  }\n"
    "\n"
    "  /**\n"
    "   * @brief Called on the control stream thread to queue an input message.",
    "    if (should_raise_input_activity) {\n"
    "      input->input_activity_event->raise(std::chrono::steady_clock::now());\n"
    "    }\n"
    "    return true;\n"
    "  }\n"
    "\n"
    "  void passthrough_queued_messages(std::shared_ptr<input_t> input) {\n"
    "    // A bounded batch prevents persistent input bursts from starving other\n"
    "    // task-pool jobs; only this task may dispatch packets for this stream.\n"
    "    constexpr std::size_t packets_per_task = 32;\n"
    "    for (std::size_t i = 0; i < packets_per_task; ++i) {\n"
    "      if (!passthrough_next_message(input)) return;\n"
    "    }\n"
    "    bool more;\n"
    "    {\n"
    "      std::lock_guard lock {input->input_queue_lock};\n"
    "      more = !input->input_queue.empty();\n"
    "      if (!more) input->input_task_scheduled = false;\n"
    "    }\n"
    "    if (more) schedule_input_packet_task(input);\n"
    "  }\n"
    "\n"
    "  /**\n"
    "   * @brief Called on the control stream thread to queue an input message.",
    "bounded per-stream worker",
)

replace_once(
    "  passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data) {\n"
    "    {\n"
    "      std::lock_guard<std::mutex> lg(input->input_queue_lock);\n",
    "  passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data) {\n"
    "    bool schedule_task = false;\n"
    "    {\n"
    "      std::lock_guard<std::mutex> lg(input->input_queue_lock);\n",
    "schedule ownership guard",
)

replace_once(
    "      input->input_queue.push_back(std::move(input_data));\n"
    "    }\n"
    "    task_pool.push(passthrough_next_message, input);\n",
    "      input->input_queue.push_back(std::move(input_data));\n"
    "      if (!input->input_task_scheduled) {\n"
    "        input->input_task_scheduled = true;\n"
    "        schedule_task = true;\n"
    "      }\n"
    "    }\n"
    "    if (schedule_task) schedule_input_packet_task(input);\n",
    "one worker per stream",
)

source.write_text(content, encoding="utf-8")
print("Ported upstream LizardByte PR #5818: serialized per-stream input; pen validation and input boost retained")
