import contextlib
import ctypes
import io
import json
import os
from pathlib import Path
import pwd
import tempfile
import unittest
from unittest.mock import patch

import hwtstamp_session as module


class SessionTests(unittest.TestCase):
    def test_ioctl_layout_and_setting_flags(self):
        self.assertEqual(ctypes.sizeof(module.Ifreq), 40)
        self.assertEqual(ctypes.sizeof(module.TimestampInfo), 44)
        with patch.object(module, 'ioctl', side_effect=lambda interface, cmd, c: c) as request:
            result = module.configure('mock0', 0, 1)
            self.assertEqual(result, {'flags': 0, 'tx_type': 0, 'rx_filter': 1})
            self.assertEqual(request.call_args.args[1], 0x89b0)

    def test_bond_configuration_acknowledges_changeable_phc(self):
        with patch.object(module, 'ioctl', side_effect=lambda interface, cmd, c: c):
            self.assertEqual(module.configure('mockbond', 0, 1, 1)['flags'], 1)

    def run_session(self, directory, fail_enable=False, fail_restore=False):
        user = pwd.getpwuid(os.getuid())
        state = Path(directory) / 'session.json'
        args = ['hwtstamp_session', '--interface', 'mock0', '--reader', user.pw_name,
                '--state', str(state), '--timeout-seconds', '1']
        def set_config(interface, tx_type, rx_filter, flags):
            self.assertEqual(flags, 1)
            if fail_enable and rx_filter == 1:
                raise RuntimeError('fabricated setup failure')
            if fail_restore and rx_filter == 0:
                raise OSError('fabricated restoration failure')
            return {'flags': flags, 'tx_type': tx_type, 'rx_filter': rx_filter}
        with patch('sys.argv', args), patch.object(module, 'configuration', return_value={'flags': 1, 'tx_type': 0, 'rx_filter': 0}), \
             patch.object(module, 'phc_index', return_value=-1), patch.object(module.os, 'geteuid', return_value=0), \
             patch.object(module.grp, 'getgrgid') as group, patch.object(module.os, 'chown'), patch.object(module.os, 'chmod'), \
             patch.object(module.time, 'monotonic', side_effect=[0, 2]), \
             patch.object(module, 'configure', side_effect=set_config) as configure, contextlib.redirect_stdout(io.StringIO()):
            group.return_value.gr_mem = []
            if fail_enable:
                with self.assertRaisesRegex(RuntimeError, 'fabricated setup failure'):
                    module.main()
            else:
                module.main()
            self.assertEqual(configure.call_args.args, ('mock0', 0, 0, 1))
            self.assertEqual(configure.call_args_list[0].args, ('mock0', 0, 1, 1))
        saved = json.loads(state.read_text())
        self.assertEqual(saved['status'], 'restore_failed' if fail_restore else 'restored')
        if fail_enable:
            self.assertEqual(saved['setup_error'], 'fabricated setup failure')

    def test_timeout_restores_original_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_session(directory)

    def test_setup_failure_restores_original_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_session(directory, fail_enable=True)

    def test_setup_error_is_not_hidden_by_restoration_error(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_session(directory, fail_enable=True, fail_restore=True)


if __name__ == '__main__':
    unittest.main()
