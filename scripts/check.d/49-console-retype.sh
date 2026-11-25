#!/usr/bin/env bash
# ITEM-427: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# A lost keystroke is not a failed proof.
#
# The three proofs that type their commands into the guest console through the
# QEMU HMP monitor ask the guest for a result and wait for it inside the
# phase's marker window.  Every keystroke travels through the kernel's local
# input path, and that path is a bounded queue: `local_input_push()` drops the
# newest event while the console has not drained the queue.  Under the parallel
# acceptance phase the guest may not be scheduled between two `sendkey`
# commands, the command never arrives, and the proof waits out its deadline for
# a result of a command the guest never saw - measured on 2026-09-16 as
# 192/193/197 seconds on test-lapic-timer-oneshot, test-pic-disable and
# test-console-history, which spent the phase's single retry on a healthy
# machine.
#
# The fix is on the proof side and it is bounded.  The console echoes every
# accepted keystroke (`redraw_input_line()` writes `\r` + `hfoc> ` + the
# current line), so a line that never appeared in the serial log was never
# delivered: `scripts/hfoc_console_command.py` retypes such a line, repeats the
# Enter until the command's own result probe succeeds, and re-presses a
# navigation key whose redraw is missing.  Nothing is retyped while the console
# shows the intended line, so a slow guest never gets a duplicated command; the
# markers and the exactness checks stay the proof's own; a budget that runs out
# still ends in the canonical HFOC-QEMU-MARKER-TIMEOUT record; and a panic
# stays fail-closed.  The number of guests is not part of the fix.
for console_retype_file in \
    scripts/hfoc_console_command.py \
    tests/test-console-retype-host.sh \
    scripts/test-qemu-lapic-timer-oneshot.sh \
    scripts/test-qemu-pic-disable.sh \
    scripts/test-qemu-console-history.sh; do
    [[ -f $console_retype_file ]] || {
        echo "ITEM-427: отсутствует $console_retype_file" >&2
        exit 1
    }
done

# The retype is bounded: an unbounded retry would hide a real failure.
for console_retype_bound in \
    'ATTEMPTS = 3' \
    'RETYPE_WINDOW = 15.0' \
    'CLEAR_KEYS = 16'; do
    grep -Fq -- "$console_retype_bound" scripts/hfoc_console_command.py || {
        echo "ITEM-427: бюджет повтора не ограничен: $console_retype_bound" >&2
        exit 1
    }
done

# The echo is the delivery witness, for a typed line and for a redraw.
for console_retype_witness in \
    'PROMPT_ECHO = "\rhfoc> "' \
    'def echo(line):' \
    'def type_line(' \
    'def submit(' \
    'def press_key('; do
    grep -Fq -- "$console_retype_witness" scripts/hfoc_console_command.py || {
        echo "ITEM-427: нет свидетеля доставки: $console_retype_witness" >&2
        exit 1
    }
done
grep -Fq 'class LostKeystrokes(Exception):' \
    scripts/hfoc_console_command.py || {
    echo "ITEM-427: исчерпанный бюджет не объявлен отдельным классом" >&2
    exit 1
}
grep -Fq 'raise SystemExit("Kernel panic during console input")' \
    scripts/hfoc_console_command.py || {
    echo "ITEM-427: паника не осталась fail-closed" >&2
    exit 1
}

# Every console-driven proof uses the same recovery, and every give-up still
# declares the class the phase retries.
for console_retype_proof in \
    scripts/test-qemu-lapic-timer-oneshot.sh \
    scripts/test-qemu-pic-disable.sh \
    scripts/test-qemu-console-history.sh; do
    for console_retype_hook in \
        'import hfoc_console_command as console_command' \
        'console_command.type_line(' \
        'console_command.submit(' \
        'except console_command.LostKeystrokes:' \
        'HFOC-QEMU-MARKER-TIMEOUT'; do
        grep -Fq -- "$console_retype_hook" "$console_retype_proof" || {
            echo "ITEM-427: $console_retype_proof не содержит $console_retype_hook" >&2
            exit 1
        }
    done
done
grep -Fq 'console_command.press_key(' \
    scripts/test-qemu-console-history.sh || {
    echo "ITEM-427: proof истории не повторяет клавиши навигации" >&2
    exit 1
}

# The markers and the exactness checks are what the retype must not weaken.
grep -Fq 'if ticks[1] <= ticks[0]:' \
    scripts/test-qemu-lapic-timer-oneshot.sh || {
    echo "ITEM-427: oneshot перестал требовать рост TICKS" >&2
    exit 1
}
for console_retype_exact in \
    scripts/test-qemu-lapic-timer-oneshot.sh \
    scripts/test-qemu-pic-disable.sh; do
    grep -Fq 'if suffix.count(help_marker) != 1:' "$console_retype_exact" || {
        echo "ITEM-427: $console_retype_exact ослабил точный подсчёт help" >&2
        exit 1
    }
done
grep -Fq 'suffix.count(version_marker) == 2 and' \
    scripts/test-qemu-console-history.sh || {
    echo "ITEM-427: history ослабил точный подсчёт version" >&2
    exit 1
}
grep -Fq 'suffix.count(help_marker) == 1 and draft_marker in suffix' \
    scripts/test-qemu-console-history.sh || {
    echo "ITEM-427: history ослабил точный подсчёт help и draft" >&2
    exit 1
}
grep -Fq 'UNKNOWN COMMAND: draft. TYPE HELP.' \
    scripts/test-qemu-console-history.sh || {
    echo "ITEM-427: history перестал запрещать submit черновика" >&2
    exit 1
}
grep -Fq 'if "\x1b[" in suffix:' scripts/test-qemu-console-history.sh || {
    echo "ITEM-427: history перестал запрещать ANSI escapes" >&2
    exit 1
}

# The fix must not be paid for by starving the phase: the measured number of
# guests stays what ITEM-412 and ITEM-426 measured (halving it did not remove
# the class and cost 32 % of the phase).
grep -Fq 'HFOC_QEMU_JOBS: "4"' .github/workflows/ci.yml || {
    echo "ITEM-427: число гостей фазы понижено" >&2
    exit 1
}

# ITEM-427 adds a third deadline inside the console-history proof (the typing
# phase that recovers a lost keystroke), so the wrapper must still dominate the
# sum of the proof's own budgets.  ITEM-426 measured the failure of the
# opposite arrangement: the wrapper killed the proof at exit 124 and the phase
# had no class record to retry.
# ITEM-457 adds a fourth budget to the same proof (the scroll burst that types
# long output and reads the records it published, plus the settle window that
# reads them), so the list and the wrapper move together by the same rule: a
# wrapper that no longer dominates the sum kills a slow run at exit 124 with no
# class record for the phase to retry.
grep -Fq 'HFOC_CONSOLE_HISTORY_TEST_TIMEOUT:-540}s' \
    scripts/test-qemu-console-history.sh || {
    echo "ITEM-427: обёртка доказательства истории не поднята под новый бюджет" >&2
    exit 1
}
console_retype_inner_budget=0
for console_retype_budget_seconds in 45 90 180 120 30 5; do
    grep -Fq "+ ${console_retype_budget_seconds}.0" \
        scripts/test-qemu-console-history.sh || {
        echo "ITEM-427: бюджет доказательства истории потерян:" \
            "${console_retype_budget_seconds}s" >&2
        exit 1
    }
    console_retype_inner_budget=$((console_retype_inner_budget +
                                 console_retype_budget_seconds))
done
(( 540 > console_retype_inner_budget )) || {
    echo "ITEM-427/457: обёртка истории (540s) не покрывает внутренние дедлайны" \
        "(${console_retype_inner_budget}s)" >&2
    exit 1
}
# A give-up on the typing phase names the budget that ran out (90 s), while the
# validation phase keeps its own 180 s window.
grep -Fq "'deadline=90s" scripts/test-qemu-console-history.sh || {
    echo "ITEM-427: отказ фазы набора не называет свой дедлайн" >&2
    exit 1
}
grep -Fq "'deadline=180s" scripts/test-qemu-console-history.sh || {
    echo "ITEM-427: окно проверки истории потеряло свой дедлайн" >&2
    exit 1
}

# The recovery is proven against a fake console, not asserted: a dropped
# keystroke is retyped and executes once, an echoed line is never duplicated,
# an exhausted budget raises LostKeystrokes, a lost navigation key is retried,
# and a panic fails at once.
bash tests/test-console-retype-host.sh || {
    echo "ITEM-427: host-тренировка повторного набора не прошла" >&2
    exit 1
}

# The rule is documented where a maintainer looks for it.
for console_retype_doc in \
    'ITEM-427' \
    'HFOC-QEMU-MARKER-TIMEOUT'; do
    grep -Fq -- "$console_retype_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-427: $console_retype_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-427' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-427" >&2
    exit 1
}

# ITEM-511: the delivery witness can be normalised for a proof whose own wake
# leaves the guest's Shift modifier set.  The console matches commands
# case-insensitively, so the upper-case echo is still the whole line, and the
# option is opt-in - the default stays the exact echo the proofs above rely on.
grep -Fq 'witness=None' scripts/hfoc_console_command.py || {
    echo "ITEM-511: witness-опция не объявлена в помощнике" >&2
    exit 1
}
grep -Fq 'witness=str.lower' scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: доказательство двух контроллеров не нормализует witness" >&2
    exit 1
}
grep -Fq 'upper-case echo confirmed with the witness (1)' \
    tests/test-console-retype-host.sh || {
    echo "ITEM-511: host-тренировка не проверяет witness" >&2
    exit 1
}
