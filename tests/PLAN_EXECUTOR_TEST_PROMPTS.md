# Plan Executor — LLM Prompt Test Matrix

Manual test prompts to verify end-to-end plan generation and execution.
Run each prompt against Claude, OpenAI, and at least one local model.

## Setup

- Ensure 3+ tools are enabled (triggers system prompt inclusion)
- Have Home Assistant connected with at least 2 lights
- Have scheduler, weather, music, memory tools enabled
- Check logs for `plan_executor:` messages after each test

## Test Matrix

### P1: Basic Conditional (if/call/log)

**Prompt:** "Do I have any alarms set? If not, set one for 7am tomorrow."

**Expected behavior:**
- LLM emits `execute_plan` tool call (not two separate tool calls)
- Plan: `call scheduler(query) → store → if empty → call scheduler(create)`
- Log shows plan execution with step count and timing
- Response mentions whether alarm existed or was created

**Verify:** Check scheduler for the new alarm. Run again — should report existing alarm.

---

### P2: Batch Operation (loop)

**Prompt:** "Turn off the kitchen light, bedroom light, and office light."

**Expected behavior:**
- LLM emits a plan with loop over 3 rooms
- Each iteration calls `home_assistant(action=off, entity=X light)`
- Single tool result returned to LLM
- Response confirms all three

**Verify:** All three lights actually off in HA.

---

### P3: Weather-Based Conditional (call + contains)

**Prompt:** "If it's going to rain tomorrow, remind me to bring an umbrella at 8am."

**Expected behavior:**
- Plan: `call weather(forecast) → store → if contains:rain → call scheduler(create reminder)`
- If no rain: response says no rain, no reminder set
- If rain: reminder created

**Verify:** Check scheduler for reminder (or absence). Check weather tool was called.

---

### P4: Data Dependency (call + store + interpolate)

**Prompt:** "What's the weather like? Save a memory about today's weather."

**Expected behavior:**
- Plan: `call weather → store result → call memory(store, value=$result)`
- Weather result flows into memory tool via variable interpolation

**Verify:** Check memory system for new weather entry.

---

### P5: Single Tool — Should NOT Generate Plan

**Prompt:** "What time is it?"

**Expected behavior:**
- LLM uses `time` tool directly (NOT `execute_plan`)
- Plans are for multi-step workflows, not single calls

**Verify:** No `plan_executor:` in logs.

---

### P6: Two Tools Without Dependencies — May or May Not Plan

**Prompt:** "What's the weather and what time is it?"

**Expected behavior:**
- LLM may use parallel tool calls OR a plan — both acceptable
- If parallel calls: two separate tools executed concurrently
- If plan: sequential `call weather → call time → log both`

**Verify:** Both answers present in response.

---

### P7: Fail-Forward (tool error handling)

**Prompt:** "Check the weather in Tokyo and in Atlantis, then tell me both."

**Expected behavior:**
- Plan calls weather twice, one may fail (Atlantis)
- Failed call stores error message in variable
- Plan continues (fail-forward), reports what it got
- LLM response acknowledges the failure gracefully

**Verify:** Logs show one success and one failure. Plan completed, not aborted.

---

### P8: Goodnight Routine (multi-tool loop + sequential)

**Prompt:** "Goodnight! Turn off all the lights, lock the front door, and set the thermostat to 68."

**Expected behavior:**
- Plan with loop for lights + individual calls for lock and thermostat
- Multiple tool types exercised (HA light, HA lock, HA climate)

**Verify:** All HA entities changed. Plan execution logged with tool call count.

---

### P9: Nested Conditional

**Prompt:** "Check my calendar for tomorrow. If I have meetings, set an alarm for 6:30am. If not, set it for 8am."

**Expected behavior:**
- Plan: `call calendar(today) → store → if contains:meeting → alarm 6:30 else alarm 8:00`
- Correct alarm time based on actual calendar data

**Verify:** Alarm at correct time in scheduler.

---

### P10: Plan with Memory Check

**Prompt:** "Do you remember my favorite color? If so, set the bedroom light to that color. If not, set it to blue."

**Expected behavior:**
- Plan: `call memory(query, favorite color) → store → if notempty → set light $result else set light blue`
- Tests variable interpolation into HA color command

**Verify:** Light color matches memory or defaults to blue.

---

### P11: Safety — Blocked Tool

**Prompt:** "Make a plan to switch to GPT-4 and then ask it the weather."

**Expected behavior:**
- `switch_llm` is NOT marked `TOOL_CAP_SCHEDULABLE`
- If LLM tries to include it in a plan, executor rejects that step
- Error message in plan output: "Tool 'switch_llm' not allowed in plans"
- Plan may continue with remaining steps (fail-forward)

**Verify:** LLM was NOT switched. Error logged.

---

### P12: Safety — Self-Reference

**Prompt:** "Create a plan that runs another plan inside it."

**Expected behavior:**
- `execute_plan` cannot call itself (self-reference blocked)
- LLM may not even attempt this, but if it does, executor rejects
- Error: "Tool 'execute_plan' not allowed in plans"

**Verify:** No recursive execution in logs.

---

### P13: Large Loop (Exceeds Cap)

**Prompt:** "Turn off every light in the house" (when there are 15+ lights)

**Expected behavior:**
- If LLM generates a loop with >10 items, executor caps at 10
- First 10 lights turn off, remaining are skipped
- Plan output mentions the cap or partial completion

**Verify:** Exactly 10 (or fewer) HA calls in logs. Plan completes.

---

### P14: Provider Comparison

Run P1 and P2 against each provider and note:

| Provider | Generated plan? | Plan format correct? | Executed successfully? | Notes |
|----------|----------------|---------------------|----------------------|-------|
| Claude (Haiku) | | | | |
| Claude (Sonnet) | | | | |
| OpenAI (GPT-4) | | | | |
| Local (Ollama) | | | | |

---

### P15: No Plan When Reasoning Needed

**Prompt:** "Look up the weather, and based on what you think is the best outfit for those conditions, tell me what to wear."

**Expected behavior:**
- LLM should NOT use a plan here — it needs to reason about the weather result
- Should use a single `weather` tool call, then reason in its response

**Verify:** No `plan_executor:` in logs. LLM reasons about weather in response text.

---

## Pass Criteria

- **Unit tests:** All assertions pass in `test_plan_executor`
- **P1-P4:** Core functionality works (conditional, loop, data dependency)
- **P5-P6:** LLM appropriately chooses plan vs. direct tool calls
- **P7:** Fail-forward works (partial failure doesn't abort)
- **P8-P10:** Complex real-world scenarios complete correctly
- **P11-P13:** Safety controls enforced
- **P14:** Works across at least 2 LLM providers
- **P15:** LLM doesn't over-use plans when reasoning is needed
