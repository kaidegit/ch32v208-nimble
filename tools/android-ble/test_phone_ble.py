import base64
import struct
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
import zlib

import phone_ble as ble


# HCI LE Extended Advertising Reports: primary and response for the test board.
PRIMARY = bytes.fromhex('3e320d01130001b1ae217b54d00100ff7fc800000000000000000018'
                        '020106100943483332563230382d4e494d424c4503ff5743')
RESPONSE = bytes.fromhex('3e2b0d011b0001b1ae217b54d00100ff7fd600000000000000000011'
                         '100943483332563230382d4e494d424c45')


def snooz(version):
    records = b''
    for packet, delta, kind in [(PRIMARY, 10, 0x10), (RESPONSE, 20, 0x10), (b'\x0c\x20\0', 30, 0x20)]:
        length = len(packet) + 1
        header = struct.pack('<HHIB', length, length, delta, kind) if version == 2 else \
            struct.pack('<HIB', length, delta, kind)
        records += header + packet
    raw = struct.pack('<BQ', version, 1000000) + zlib.compress(records)
    return '--- BEGIN:BTSNOOP_LOG_SUMMARY ---\n' + base64.b64encode(raw).decode() + \
           '\n--- END:BTSNOOP_LOG_SUMMARY ---'


class PhoneDiagnosticsTests(unittest.TestCase):
    def test_hci_primary_response_and_rssi(self):
        for version in (1, 2):
            with self.subTest(version=version):
                result = ble.hci_summary(snooz(version), 'CH32V208-NIMBLE', None)
                self.assertEqual(result['records'], 3)
                self.assertEqual(len(result['targets']), 1)
                target = result['targets'][0]
                self.assertEqual(target['address'], 'D0:54:7B:21:AE:B1')
                self.assertEqual((target['primary_reports'], target['scan_response_reports']), (1, 1))
                self.assertEqual((target['rssi_min'], target['rssi_max']), (-56, -42))

    def test_hci_baseline_excludes_cached_packets(self):
        result = ble.hci_summary(snooz(2), 'CH32V208-NIMBLE', None, 999950)
        self.assertEqual(result['targets'][0]['primary_reports'], 0)
        self.assertEqual(result['targets'][0]['scan_response_reports'], 1)
        self.assertEqual(ble.hci_summary(snooz(2), 'CH32V208-NIMBLE', None, 1000000)['targets'], [])

    def test_hci_address_selection_and_missing_snoop(self):
        result = ble.hci_summary(snooz(2), 'irrelevant', 'd0:54:7b:21:ae:b1')
        self.assertEqual(len(result['targets']), 1)
        self.assertEqual(ble.hci_summary('not present', 'CH32V208-NIMBLE', None), {'available': False})

    def test_ui_characteristic_values_do_not_cross_rows(self):
        root = ET.fromstring(f'''<hierarchy><node>
          <node><node resource-id="{ble.PKG}:id/action_read"/>
            <node resource-id="{ble.PKG}:id/uuid" text="UUID: {ble.SERVICE[:-2]}02"/>
            <node resource-id="{ble.PKG}:id/value" text="Value: (0x) 5E-00-00-00"/></node>
          <node><node resource-id="{ble.PKG}:id/action_read"/>
            <node resource-id="{ble.PKG}:id/uuid" text="UUID: {ble.SERVICE[:-2]}03"/>
            <node resource-id="{ble.PKG}:id/value" text="Value: "/></node>
        </node></hierarchy>''')
        self.assertEqual(ble.value_bytes(root, ble.SERVICE[:-2] + '02'), b'\x5e\0\0\0')
        self.assertEqual(ble.value_bytes(root, ble.SERVICE[:-2] + '03'), b'')
        with self.assertRaises(RuntimeError):
            ble.value_bytes(root, ble.SERVICE[:-2] + '04')

    def test_filter_controls_cannot_be_tapped(self):
        phone = object.__new__(ble.Phone)
        phone.run = lambda *a, **kw: self.fail('ADB must not be called for filter controls')
        for control in ('header', 'action_clear_filter', 'filter', 'action_clear'):
            node = ET.Element('node', {'resource-id': ble.PKG + ':id/' + control,
                                      'clickable': 'true', 'enabled': 'true', 'bounds': '[0,0][100,100]'})
            with self.assertRaises(RuntimeError):
                phone.tap(node)

    def test_failed_connection_is_not_retried(self):
        root = ET.fromstring(f'''<hierarchy>
          <node resource-id="android:id/text2" text="D0:54:7B:21:AE:B1" selected="true"/>
          <node resource-id="{ble.PKG}:id/connection_state" text="DISCONNECTED"/>
        </hierarchy>''')
        phone = object.__new__(ble.Phone)
        phone.ui = lambda *args: root
        phone.screenshot = lambda *args: None
        phone.tap = lambda *args: self.fail('A failed request must not trigger another CONNECT tap')
        report = {}
        with patch('phone_ble.time.sleep'), patch('phone_ble.emit'):
            code = phone.connection(root, 'D0:54:7B:21:AE:B1', report, requested=True)
        self.assertEqual(code, 5)
        self.assertFalse(report['success'])

    def test_gatt_selects_test_service_after_standard_services(self):
        root = ET.fromstring(f'''<hierarchy>
          <node resource-id="android:id/text2" text="D0:54:7B:21:AE:B1" selected="true"/>
          <node resource-id="{ble.PKG}:id/connection_state" text="CONNECTED"/>
          <node resource-id="{ble.PKG}:id/service_main"><node text="0x1800"/></node>
          <node resource-id="{ble.PKG}:id/service_main"><node text="0x1801"/></node>
          <node resource-id="{ble.PKG}:id/service_main"><node text="{ble.SERVICE}"/></node>
        </hierarchy>''')
        target_service = list(root)[-1]
        expanded = ET.fromstring(ET.tostring(root))
        expanded.append(ET.fromstring(f'''<node>
          <node resource-id="{ble.PKG}:id/uuid" text="{ble.SERVICE[:-2]}02"/>
          <node resource-id="{ble.PKG}:id/action_read"/>
          <node resource-id="{ble.PKG}:id/value" text="Value: (0x) 01-00-00-00"/>
        </node>'''))
        phone = object.__new__(ble.Phone)
        tapped = []
        phone.tap = tapped.append
        phone.ui = lambda *args: expanded
        phone.screenshot = lambda *args: None
        report = {}
        with patch('phone_ble.time.sleep'), patch('phone_ble.emit'):
            code = phone.gatt(root, 'D0:54:7B:21:AE:B1', ble.SERVICE[:-2] + '02', None, report)
        self.assertIs(tapped[0], target_service)
        self.assertEqual(code, 0)
        self.assertEqual(report['uptime_seconds'], 1)


if __name__ == '__main__':
    unittest.main()
