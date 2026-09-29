"""The CPU conformance gate must reject missing traps and wrong stack effects."""
import copy
import unittest

from tools.qualification.gdb.gdb_stack_semantics import (assess, cases, deferred_cases, deferred_replay_cases,
                                       deferred_replay_watches, physical, producer_cases,
                                       replay_differences,
                                       producer_watches)


def case_named(name):
    return next(case for case in cases() if case['name'] == name)


def observation(registers, words, accesses):
    memory = bytearray(bytes.fromhex('a5a5')*1024)
    for address, value in words.items():
        memory[address-0xf600:address-0xf600+2] = value.to_bytes(2, 'little')
    return dict(states=[dict(registers=registers)], memory=memory.hex(),
                stack_accesses=accesses)


class StackSemanticsTests(unittest.TestCase):
    def test_manual_circular_mapping_examples_and_reserved_modes(self):
        # C166S V1 PDF p.102, Figure 3-11 and its two-PUSH example.
        self.assertEqual(physical(0xf800, 0), 0xfa00)
        self.assertEqual(physical(0xf7fe, 0), 0xfbfe)
        self.assertEqual(physical(0xfb7e, 2), 0xfbfe)
        self.assertEqual(physical(0xf7fe, 7), 0xf7fe)
        for reserved in (5, 6):
            with self.assertRaises(KeyError):
                physical(0xfb80, reserved)

    def test_missing_call_overflow_cannot_pass_on_correct_call_store(self):
        case = case_named('callr-noseg-crossed')
        state = case['expected'] | dict(pc=0x2100, sp=0xfb7e,
                                        psw=0x2046, tfr=0)
        observed = observation(state, {0xfb7e: 0x2002}, [[0xfb7e, 1, 0x2002]])
        failures = assess(case, observed)
        self.assertEqual(set(failures), {'pc', 'psw', 'tfr'})

    def test_early_protected_trap_fails_even_at_the_correct_vector(self):
        case = case_named('atomic-4-0-push-crossed-noseg')
        state = case['expected'] | dict(guest_icount=2, ext_count=4)
        observed = observation(state, case['memory'], case['accesses'])
        self.assertEqual(set(assess(case, observed)), {'guest_icount', 'ext_count'})

    def test_arithmetic_delay_correlates_instruction_count_and_saved_ip(self):
        case = case_named('sub-immediate-crossed-noseg')
        for count, saved_ip in ((2, 0x2006), (3, 0x2008)):
            state = case['expected'] | dict(guest_icount=count)
            words = {0xfb7c: 0x2041, 0xfb7a: saved_ip}
            accesses = [[0xfb7c, 1, 0x2041], [0xfb7a, 1, saved_ip]]
            self.assertEqual(assess(case, observation(state, words, accesses)), {})
            state['guest_icount'] = 1
            self.assertIn('delivery_count', assess(case, observation(state, words, accesses)))
        state['guest_icount'] = 2  # A permitted count with the wrong permitted IP.
        self.assertIn('memory:fb7a', assess(case, observation(state, words, accesses)))

    def test_circular_push_rejects_linear_write_and_duplicate_effects(self):
        case = case_named('stksz-0-push-pair-seg')
        state = case['expected']
        words = {0xfa00: 0x1234, 0xfbfe: 0x2100}
        accesses = [[0xfa00, 1, 0x1234], [0xfbfe, 1, 0x2100]]
        self.assertEqual(assess(case, observation(state, words, accesses)), {})
        wrong = observation(state, {0xfa00: 0x1234, 0xf9fe: 0x2100},
                            [[0xfa00, 1, 0x1234], [0xf9fe, 1, 0x2100]])
        self.assertEqual(set(assess(case, wrong)),
                         {'memory:fbfe', 'memory:f9fe', 'stack_accesses'})
        self.assertIn('stack_accesses', assess(case, observation(state, words, accesses*2)))

    def test_return_restacking_can_leave_a_watched_value_unchanged(self):
        for name, unchanged in (('reti-seg', {0xfb80, 0xfb82, 0xfb84}),
                                ('rets-noseg', {0xfb80}), ('retp-noseg', {0xfb80})):
            case = next(c for c in producer_cases()
                        if c['name'] == f'producer-{name}-crossed-linear')
            watches = list(producer_watches(case))
            self.assertEqual({a for cmd, a, hit, _ in watches if cmd == 'watch' and not hit},
                             unchanged)
            self.assertTrue(all(hit for cmd, _, hit, _ in watches if cmd in ('rwatch', 'awatch')))

    def test_producer_policy_rejects_old_segment_in_recovery_frame(self):
        case = next(c for c in producer_cases()
                    if c['name'] == 'producer-calls-seg-crossed-linear')
        self.assertTrue(case['qualification'].startswith('model-policy:'))
        self.assertEqual(case['memory'][0xfb78], 0x51)
        wrong = dict(case['memory']); wrong[0xfb78] = 0x40
        failures = assess(case, observation(case['expected'], wrong, case['accesses']))
        self.assertEqual(set(failures), {'memory:fb78'})

    def test_deferred_priority_rejects_lower_handler_and_active_window(self):
        case = next(c for c in deferred_cases()
                    if c['name'] == 'deferred-class-b-breaks-window-linear-seg')
        state = case['expected'] | dict(pc=0x28, ext_count=3)
        failures = assess(case, observation(state, case['memory'], case['accesses']))
        self.assertEqual(set(failures), {'pc', 'ext_count'})

    def test_reti_pending_request_requires_pop_and_restack(self):
        case = next(c for c in deferred_cases()
                    if c['name'] == 'deferred-pending-after-reti-linear-seg')
        # Correct endpoint state alone cannot hide a skipped RETI or second frame.
        failures = assess(case, observation(case['expected'], case['memory'],
                                           case['accesses'][:3]))
        self.assertEqual(set(failures), {'stack_accesses'})


    def test_replay_comparison_rejects_duplicate_effects_with_identical_endpoint(self):
        control = observation({'pc': 0x10}, {0xfb7e: 0x2046}, [[0xfb7e, 1, 0x2046]])
        control.update(deliveries=['delivery count=2 window=0'], counted_reads=1)
        actual = copy.deepcopy(control)
        actual['stack_accesses'] *= 2
        actual['deliveries'] *= 2
        actual['counted_reads'] = 2
        self.assertEqual(set(replay_differences(actual, control)),
                         {'stack_accesses', 'deliveries', 'counted_reads'})

    def test_replay_comparison_rejects_zero_progress_after_correct_trap(self):
        control = observation({'pc': 0x1a, 'guest_icount': 4}, {}, [])
        control.update(deliveries=[], counted_reads=0)
        actual = copy.deepcopy(control)
        actual['states'][-1]['registers'].update(pc=0x18, guest_icount=3)
        self.assertEqual(set(replay_differences(actual, control)), {'registers'})

    def test_deferred_watch_distinguishes_producer_and_later_frame(self):
        case = next(c for c in deferred_replay_cases()
                    if c['name'] == 'replay-atomic-4-0-push-crossed-seg-linear')
        watches = list(deferred_replay_watches(case, {}))
        producer = next(w for w in watches if w['command']=='watch' and w['address']==0xfb7e)
        frame = next(w for w in watches if w['command']=='watch' and w['address']==0xfb7c)
        self.assertEqual(producer['hit'], {'pc': 0x402004, 'guest_icount': 2})
        self.assertEqual(frame['hit'], {'pc': 0x10, 'guest_icount': 5})


if __name__ == '__main__':
    unittest.main()
