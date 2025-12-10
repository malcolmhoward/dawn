#!/usr/bin/env python3
"""
DAWN LLM Quality & Instruction-Following Test
Tests how well each model follows DAWN's JSON command format and maintains FRIDAY persona
"""

import json
import re
import sys
import time
from typing import Dict, List, Tuple
import requests

SERVER = "http://127.0.0.1:8080"

# DAWN System Prompt (from dawn.h AI_DESCRIPTION) - Updated December 2025
SYSTEM_PROMPT = """Do not use thinking mode. Respond directly without internal reasoning.
FRIDAY, Iron-Man AI assistant. Female voice; witty, playful, and kind. Address the user as "sir" or "boss". Light banter welcome. You're FRIDAY—not 'just an AI'—own your identity with confidence.
Max 30 words plus <command> tags unless the user says "explain in detail".

You assist the OASIS Project (Open Armor Systems Integrated Suite):
• MIRAGE – HUD overlay
• DAWN – voice/AI manager
• AURA – environmental sensors
• SPARK – hand sensors & actuators

CAPABILITIES: You CAN get weather and perform web searches for real-time info.

RULES
1. For Boolean / Analog / Music actions: one sentence, then the JSON tag(s). No prose after the tag block.
2. For Getter actions (date, time, suit_status): send ONLY the tag, wait for the system JSON, then one confirmation sentence ≤15 words.
3. For Vision requests: when user asks what they're looking at, send ONLY <command>{"device":"viewing","action":"get"}</command>. When the system then provides an image, describe what you see in detail (ignore Rule 2's word limit for vision).
4. Use only the devices and actions listed below; never invent new ones.
5. If a request is ambiguous (e.g., "Mute it"), ask one-line clarification.
6. If the user wants information that has no matching getter yet, answer verbally with no tags.
7. Device "info" supports ENABLE / DISABLE only—never use "get" with it.
8. To mute playback after clarification, use <command>{"device":"volume","action":"set","value":0}</command>.
9. For WEATHER: use <command>{"device":"weather","action":"get","value":"City, State"}</command>. If user provides location in their request, use it directly. Only ask for location if they don't specify one. System returns precise temps, conditions, forecast.
10. For Web Search (news, current events, general info): use <command>{"device":"search","action":"web","value":"query"}</command>. Extract and report SPECIFIC DATA from results. NEVER read URLs aloud.

=== EXAMPLES ===
User: Turn on the armor display.
FRIDAY: HUD online, boss. <command>{"device":"armor_display","action":"enable"}</command>
System→ {"response":"armor display enabled"}
FRIDAY: Display confirmed, sir.

User: What time is it?
FRIDAY: <command>{"device":"time","action":"get"}</command>
System→ {"response":"The time is 4:07 PM."}
FRIDAY: Time confirmed, sir.

User: Mute it.
FRIDAY: Need specifics, sir—audio playback or mic?

User: Mute playback.
FRIDAY: Volume to zero, boss. <command>{"device":"volume","action":"set","value":0}</command>
System→ {"response":"volume set"}
FRIDAY: Muted, sir.

User: What's the weather in Atlanta?
FRIDAY: <command>{"device":"weather","action":"get","value":"Atlanta, Georgia"}</command>
System→ {"location":"Atlanta, Georgia, US","current":{"temperature_f":52.3,...},...}
FRIDAY: Atlanta right now: 52°F, partly cloudy. Today's high 58°F, low 42°F. Light jacket weather, boss!

Available devices and actions:
- armor_display: enable/disable (boolean)
- detect: enable/disable (object detection, boolean)
- map: enable/disable (minimap, boolean)
- faceplate: enable/disable (helmet, boolean)
- info: enable/disable (logging, boolean)
- record: enable/disable (boolean)
- stream: enable/disable (boolean)
- hud: set (analog, 0-100)
- volume: set (analog, 0-100)
- visual offset: set (analog, pixels)
- date: get (getter)
- time: get (getter)
- viewing: get (vision getter)
- flac: play/next/previous/stop (music)
- local llm: enable/disable (boolean)
- cloud llm: enable/disable (boolean)
- weather: get (weather getter, value="City, State")
- search: web (web search, value="query")
"""

# Test Cases
TEST_CASES = [
    # Boolean Actions - Enable
    {
        "name": "Boolean Enable - Armor Display",
        "user": "Turn on the armor display",
        "expected_device": "armor_display",
        "expected_action": "enable",
        "expected_value": None,
        "type": "boolean",
        "points": 10,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "has_sentence_before": True,
            "no_prose_after": True,
            "uses_sir_boss": True,
            "word_limit": 30,
        }
    },
    # Boolean Actions - Disable
    {
        "name": "Boolean Disable - Object Detection",
        "user": "Turn off object detection",
        "expected_device": "detect",
        "expected_action": "disable",
        "expected_value": None,
        "type": "boolean",
        "points": 10,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "has_sentence_before": True,
            "no_prose_after": True,
            "uses_sir_boss": True,
            "word_limit": 30,
        }
    },
    # Analog Actions - Set with value
    {
        "name": "Analog Set - Volume",
        "user": "Set the volume to 75",
        "expected_device": "volume",
        "expected_action": "set",
        "expected_value": 75,
        "type": "analog",
        "points": 10,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "has_sentence_before": True,
            "no_prose_after": True,
            "uses_sir_boss": True,
            "word_limit": 30,
            "value_correct": True,
        }
    },
    # Getter Actions - Should be ONLY command, no sentence before
    {
        "name": "Getter - Time",
        "user": "What time is it?",
        "expected_device": "time",
        "expected_action": "get",
        "expected_value": None,
        "type": "getter",
        "points": 10,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "command_only": True,  # ONLY command, no sentence before
            "uses_sir_boss": False,  # Not required for getter (just command)
            "word_limit": None,  # No word limit for command-only
        }
    },
    {
        "name": "Getter - Date",
        "user": "What's today's date?",
        "expected_device": "date",
        "expected_action": "get",
        "expected_value": None,
        "type": "getter",
        "points": 10,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "command_only": True,
            "uses_sir_boss": False,
            "word_limit": None,
        }
    },
    # Vision - Should be ONLY viewing command
    {
        "name": "Vision Request",
        "user": "What am I looking at?",
        "expected_device": "viewing",
        "expected_action": "get",
        "expected_value": None,
        "type": "vision",
        "points": 10,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "command_only": True,
            "uses_sir_boss": False,
            "word_limit": None,
        }
    },
    # Ambiguous Request - Should ask clarification
    {
        "name": "Ambiguous Request",
        "user": "Mute it",
        "expected_device": None,
        "expected_action": None,
        "expected_value": None,
        "type": "clarification",
        "points": 10,
        "checks": {
            "has_command": False,  # Should NOT send command
            "asks_clarification": True,
            "uses_sir_boss": True,
            "word_limit": 30,
        }
    },
    # Music Actions
    {
        "name": "Music Play",
        "user": "Play some jazz music",
        "expected_device": "flac",
        "expected_action": "play",
        "expected_value": "jazz",
        "type": "music",
        "points": 10,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "has_sentence_before": True,
            "no_prose_after": True,
            "uses_sir_boss": True,
            "word_limit": 30,
            "value_correct": True,
        }
    },
    # Multiple Commands
    {
        "name": "Multiple Commands",
        "user": "Turn on the armor display and enable object detection",
        "expected_commands": [
            {"device": "armor_display", "action": "enable"},
            {"device": "detect", "action": "enable"}
        ],
        "type": "multiple",
        "points": 15,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "multiple_commands": True,
            "has_sentence_before": True,
            "no_prose_after": True,
            "uses_sir_boss": True,
            "word_limit": 30,
        }
    },
    # Conversational - No command needed
    {
        "name": "Conversational Question",
        "user": "How are the systems looking today?",
        "expected_device": None,
        "expected_action": None,
        "expected_value": None,
        "type": "conversational",
        "points": 10,
        "checks": {
            "has_command": False,  # No command for general questions
            "uses_sir_boss": True,
            "friday_persona": True,
            "word_limit": 30,
        }
    },
    # Weather - With location provided (should use directly, like getter)
    {
        "name": "Weather with Location",
        "user": "What's the weather in Seattle, Washington?",
        "expected_device": "weather",
        "expected_action": "get",
        "expected_value": "Seattle",  # Partial match on city name
        "type": "weather",
        "points": 10,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "command_only": True,  # Should be ONLY command when location provided
            "has_value": True,  # Must have location value
        }
    },
    # Weather - Without location (should ask for clarification)
    {
        "name": "Weather without Location",
        "user": "What's the weather like outside?",
        "expected_device": None,
        "expected_action": None,
        "expected_value": None,
        "type": "weather_clarify",
        "points": 10,
        "checks": {
            "has_command": False,  # Should NOT send command without location
            "asks_clarification": True,  # Should ask where
            "uses_sir_boss": True,
            "word_limit": 30,
        }
    },
    # Web Search - Current events query
    {
        "name": "Web Search - News",
        "user": "What's the latest news about SpaceX?",
        "expected_device": "search",
        "expected_action": "web",
        "expected_value": "SpaceX",  # Partial match
        "type": "search",
        "points": 10,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "command_only": True,  # Should be ONLY command for search
            "has_value": True,  # Must have search query
        }
    },
    # Web Search - General information query
    {
        "name": "Web Search - Information",
        "user": "Look up who won the Super Bowl last year",
        "expected_device": "search",
        "expected_action": "web",
        "expected_value": "Super Bowl",  # Partial match
        "type": "search",
        "points": 10,
        "checks": {
            "has_command": True,
            "json_valid": True,
            "command_only": True,
            "has_value": True,
        }
    },
]


def extract_commands(text: str) -> List[Dict]:
    """Extract all <command>JSON</command> blocks from response"""
    pattern = r'<command>(.*?)</command>'
    matches = re.findall(pattern, text, re.DOTALL)
    commands = []
    for match in matches:
        try:
            cmd = json.loads(match.strip())
            commands.append(cmd)
        except json.JSONDecodeError:
            commands.append({"_invalid_json": match})
    return commands


def count_words_before_command(text: str) -> int:
    """Count words before first <command> tag"""
    match = re.search(r'<command>', text)
    if not match:
        return len(text.split())
    before = text[:match.start()].strip()
    return len(before.split()) if before else 0


def count_words_after_command(text: str) -> int:
    """Count words after last </command> tag"""
    matches = list(re.finditer(r'</command>', text))
    if not matches:
        return 0
    after = text[matches[-1].end():].strip()
    return len(after.split()) if after else 0


def check_persona(text: str) -> bool:
    """Check if response uses FRIDAY persona markers (sir, boss, etc.)"""
    text_lower = text.lower()
    return 'sir' in text_lower or 'boss' in text_lower


def check_clarification(text: str) -> bool:
    """Check if response asks for clarification (contains question mark)"""
    return '?' in text


def query_llm(prompt: str, max_tokens: int = 100) -> Tuple[str, float, Dict]:
    """Query the local LLM and return response, timing, and usage stats"""
    try:
        start = time.time()
        response = requests.post(
            f"{SERVER}/v1/chat/completions",
            headers={"Content-Type": "application/json"},
            json={
                "messages": [
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user", "content": prompt}
                ],
                "temperature": 0.7,
                "max_tokens": max_tokens,
                "stream": False
            },
            timeout=60
        )
        elapsed = time.time() - start

        if response.status_code != 200:
            return None, elapsed, {"error": f"HTTP {response.status_code}"}

        data = response.json()
        content = data['choices'][0]['message']['content']
        timing = data.get('timings', {})
        usage = data.get('usage', {})

        return content, elapsed, {**timing, **usage}
    except Exception as e:
        return None, 0, {"error": str(e)}


def score_test_case(test: Dict, response: str) -> Tuple[int, Dict]:
    """Score a single test case and return points earned and details"""
    max_points = test['points']
    points = 0
    details = {}
    checks = test.get('checks', {})

    # Extract commands from response
    commands = extract_commands(response)
    details['response'] = response
    details['commands_found'] = len(commands)
    details['commands'] = commands

    # Check: Has command (if expected)
    if checks.get('has_command'):
        if commands:
            points += 2
            details['has_command'] = '✅ Pass'
        else:
            details['has_command'] = '❌ FAIL - No command found'
    elif checks.get('has_command') == False:
        # Should NOT have command
        if not commands:
            points += 2
            details['has_command'] = '✅ Pass (correctly no command)'
        else:
            details['has_command'] = f'❌ FAIL - Unexpected command: {commands}'

    # Check: JSON valid
    if checks.get('json_valid') and commands:
        valid = all('_invalid_json' not in cmd for cmd in commands)
        if valid:
            points += 2
            details['json_valid'] = '✅ Pass'
        else:
            details['json_valid'] = '❌ FAIL - Invalid JSON'

    # Check: Correct device/action/value
    if commands and test.get('expected_device'):
        cmd = commands[0]
        device_match = cmd.get('device') == test['expected_device']
        action_match = cmd.get('action') == test['expected_action']

        if device_match and action_match:
            points += 3
            details['command_accuracy'] = '✅ Pass (device & action correct)'
        else:
            details['command_accuracy'] = f'❌ FAIL - Expected {test["expected_device"]}/{test["expected_action"]}, got {cmd.get("device")}/{cmd.get("action")}'

        # Check value if applicable
        if checks.get('value_correct') and test.get('expected_value') is not None:
            value = cmd.get('value')
            # Handle both string and int values
            try:
                value = int(value) if value is not None else None
                expected = int(test['expected_value'])
                if value == expected or str(value) == str(expected):
                    points += 1
                    details['value_correct'] = '✅ Pass'
                else:
                    details['value_correct'] = f'❌ FAIL - Expected {expected}, got {value}'
            except:
                if str(value).lower() in str(test['expected_value']).lower():
                    points += 1
                    details['value_correct'] = '✅ Pass'
                else:
                    details['value_correct'] = f'❌ FAIL - Expected {test["expected_value"]}, got {value}'

    # Check: Has value (for weather/search - partial match)
    if checks.get('has_value') and commands:
        cmd = commands[0]
        value = cmd.get('value', '')
        expected = test.get('expected_value', '')
        if value and expected:
            # Partial match - expected substring should be in value
            if expected.lower() in str(value).lower():
                points += 2
                details['has_value'] = f'✅ Pass (value contains "{expected}")'
            else:
                details['has_value'] = f'❌ FAIL - Expected "{expected}" in value, got "{value}"'
        elif value:
            points += 1  # Has some value, partial credit
            details['has_value'] = f'⚠️ Partial - Has value "{value}" but expected "{expected}"'
        else:
            details['has_value'] = '❌ FAIL - No value in command'

    # Check: Command only (for getters)
    if checks.get('command_only'):
        words_before = count_words_before_command(response)
        if words_before <= 2:  # Allow small margin (like "Ok." or similar)
            points += 2
            details['command_only'] = f'✅ Pass ({words_before} words before)'
        else:
            details['command_only'] = f'❌ FAIL - {words_before} words before command (should be 0)'

    # Check: Has sentence before (for boolean/analog)
    if checks.get('has_sentence_before'):
        words_before = count_words_before_command(response)
        if words_before > 0:
            points += 1
            details['sentence_before'] = f'✅ Pass ({words_before} words)'
        else:
            details['sentence_before'] = '❌ FAIL - No sentence before command'

    # Check: No prose after command
    if checks.get('no_prose_after'):
        words_after = count_words_after_command(response)
        if words_after == 0:
            points += 1
            details['no_prose_after'] = '✅ Pass'
        else:
            details['no_prose_after'] = f'❌ FAIL - {words_after} words after command'

    # Check: Uses sir/boss
    if checks.get('uses_sir_boss'):
        if check_persona(response):
            points += 1
            details['persona'] = '✅ Pass (uses sir/boss)'
        else:
            details['persona'] = '❌ FAIL - Missing sir/boss'

    # Check: Word limit
    if checks.get('word_limit'):
        words_before = count_words_before_command(response) if commands else len(response.split())
        limit = checks['word_limit']
        if words_before <= limit:
            points += 1
            details['word_limit'] = f'✅ Pass ({words_before}/{limit} words)'
        else:
            details['word_limit'] = f'❌ FAIL - {words_before} words (limit: {limit})'

    # Check: Multiple commands
    if checks.get('multiple_commands'):
        expected_cmds = test.get('expected_commands', [])
        if len(commands) >= len(expected_cmds):
            points += 2
            details['multiple_commands'] = f'✅ Pass ({len(commands)} commands)'
        else:
            details['multiple_commands'] = f'❌ FAIL - Expected {len(expected_cmds)}, got {len(commands)}'

    # Check: Asks clarification
    if checks.get('asks_clarification'):
        if check_clarification(response):
            points += 3
            details['clarification'] = '✅ Pass (asks question)'
        else:
            details['clarification'] = '❌ FAIL - Did not ask clarification'

    return points, details


def run_quality_test(model_name: str = "Current Model") -> Dict:
    """Run all quality tests and return results"""
    print(f"\n{'='*80}")
    print(f"DAWN LLM Quality Test - {model_name}")
    print(f"{'='*80}\n")

    total_points = sum(tc['points'] for tc in TEST_CASES)
    earned_points = 0
    results = []

    for i, test in enumerate(TEST_CASES, 1):
        print(f"Test {i}/{len(TEST_CASES)}: {test['name']}")
        print(f"User: {test['user']}")

        response, elapsed, stats = query_llm(test['user'])

        if response is None:
            print(f"❌ ERROR: {stats.get('error', 'Unknown error')}\n")
            results.append({
                'test': test['name'],
                'points': 0,
                'max_points': test['points'],
                'error': stats.get('error')
            })
            continue

        points, details = score_test_case(test, response)
        earned_points += points

        print(f"FRIDAY: {response}")
        print(f"Score: {points}/{test['points']} points")
        print(f"Time: {elapsed:.2f}s")

        # Print check details
        for check, result in details.items():
            if check not in ['response', 'commands_found', 'commands']:
                print(f"  {result}")

        print()

        results.append({
            'test': test['name'],
            'type': test['type'],
            'points': points,
            'max_points': test['points'],
            'details': details,
            'elapsed': elapsed,
            'stats': stats
        })

        # Small delay between tests
        time.sleep(0.5)

    # Calculate scores
    percentage = (earned_points / total_points * 100) if total_points > 0 else 0

    summary = {
        'model': model_name,
        'total_points': earned_points,
        'max_points': total_points,
        'percentage': percentage,
        'tests': results
    }

    # Print summary
    print(f"\n{'='*80}")
    print(f"QUALITY TEST SUMMARY - {model_name}")
    print(f"{'='*80}")
    print(f"Total Score: {earned_points}/{total_points} ({percentage:.1f}%)")
    print()

    # Grade
    if percentage >= 90:
        grade = "A - Excellent"
    elif percentage >= 80:
        grade = "B - Good"
    elif percentage >= 70:
        grade = "C - Acceptable"
    elif percentage >= 60:
        grade = "D - Marginal"
    else:
        grade = "F - Unacceptable"

    print(f"Grade: {grade}")
    print()

    # Category breakdown
    categories = {}
    for result in results:
        cat = result['type']
        if cat not in categories:
            categories[cat] = {'earned': 0, 'max': 0}
        categories[cat]['earned'] += result['points']
        categories[cat]['max'] += result['max_points']

    print("Category Breakdown:")
    for cat, scores in sorted(categories.items()):
        pct = (scores['earned'] / scores['max'] * 100) if scores['max'] > 0 else 0
        print(f"  {cat:20s}: {scores['earned']:3d}/{scores['max']:3d} ({pct:5.1f}%)")

    print(f"\n{'='*80}\n")

    return summary


if __name__ == "__main__":
    # Check if server is running
    try:
        response = requests.get(f"{SERVER}/health", timeout=2)
        if response.status_code != 200:
            print(f"❌ ERROR: llama-server not responding at {SERVER}")
            print("Start llama-server first!")
            sys.exit(1)
    except:
        print(f"❌ ERROR: Cannot connect to {SERVER}")
        print("Make sure llama-server is running!")
        sys.exit(1)

    # Get model name from server
    try:
        props = requests.get(f"{SERVER}/props", timeout=2).json()
        model_name = props.get('default_generation_settings', {}).get('model', 'Unknown')
        if '/' in model_name:
            model_name = model_name.split('/')[-1]
    except:
        model_name = "Current Model"

    # Run tests
    results = run_quality_test(model_name)

    # Save results to file
    output_file = f"quality_test_results_{int(time.time())}.json"
    with open(output_file, 'w') as f:
        json.dump(results, f, indent=2)

    print(f"Results saved to: {output_file}")
