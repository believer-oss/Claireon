"""Astral MCP raw-key ingress tests; no editor or subprocess required."""

import io
import json
import unittest
from unittest import mock

import claireon_proxy as proxy


class AstralMatrixMCPTransport(unittest.TestCase):
    def rpc(self, arguments, name="astral_matrix_author"):
        return ('{"jsonrpc":"2.0","id":41,"method":"tools/call",'
                '"params":{"name":' + json.dumps(name) + ',"arguments":' + arguments + '}}').encode()

    def handler(self, raw):
        handler = mock.Mock()
        handler.path = "/mcp"
        handler.headers = {"Content-Length": str(len(raw))}
        handler.rfile = io.BytesIO(raw)
        handler.server.server_address = ("127.0.0.1", 0)
        return handler

    def test_exact_and_escaped_duplicates_rejected_before_dispatch(self):
        for arguments in (
            r'{"ember_cost":{"ember":1,"ember":2}}',
            r'{"nodes":[{"ember_cost":{"ember":1,"\u0065mber":2}}]}',
            r'{"nested":{"\"":1,"\u0022":2}}',
            r'{"nested":{"":1,"":2}}',
        ):
            with self.subTest(arguments=arguments), mock.patch.object(proxy, "_handle_mcp_payload") as dispatch:
                handler = self.handler(self.rpc(arguments))
                proxy.McpHandler.do_POST(handler)
                dispatch.assert_not_called()
                status, response = handler._respond_json.call_args.args
                self.assertEqual(status, 200)
                self.assertEqual(response["id"], 41)
                self.assertEqual(response["error"]["code"], -32602)

    def test_case_distinct_keys_survive_for_ue_guard(self):
        handler = self.handler(self.rpc(r'{"ember_cost":{"Ember":1,"ember":2}}'))
        with mock.patch.object(proxy, "_handle_mcp_payload", return_value={"result": {}}) as dispatch:
            proxy.McpHandler.do_POST(handler)
        costs = dispatch.call_args.args[0]["params"]["arguments"]["ember_cost"]
        self.assertEqual(costs, {"Ember": 1, "ember": 2})

    def test_all_eight_tools_checked(self):
        for name in proxy._ASTRAL_JSON_TOOLS:
            with self.subTest(name=name), self.assertRaises(proxy._AstralJsonKeyError):
                proxy._parse_mcp_request(self.rpc('{"x":1,"x":2}', name))

    def test_ambiguous_routing_cannot_discard_astral_candidate(self):
        requests = (
            '{"method":"tools/call","params":{"name":"astral_matrix_save","name":"other"}}',
            '{"method":"tools/call","params":{"name":"other","name":"astral_matrix_save"}}',
            '{"params":{"name":"astral_matrix_save"},"params":{"name":"other"},"method":"tools/call"}',
            '{"method":"tools/call","method":"tools/list","params":{"name":"astral_matrix_save"}}',
            r'{"method":"tools/call","p\u0061rams":{"name":"astral_matrix_save","arguments":{"x":1,"x":2}}}',
        )
        for raw in requests:
            with self.subTest(raw=raw), self.assertRaises(proxy._AstralJsonKeyError):
                proxy._parse_mcp_request(raw.encode())

    def test_separate_objects_and_json_escapes_remain_valid(self):
        arguments = r'{"nodes":[{"ember":1},{"ember":2}],"text":"\"ember\":1,\"ember\":2 {} [] \\ end","\u0061":1,"a/b":2}'
        parsed = proxy._parse_mcp_request(self.rpc(arguments))["params"]["arguments"]
        self.assertEqual(parsed, json.loads(arguments))
        nested = '{"value":' * 40 + '{"ember":1}' + '}' * 40
        self.assertEqual(proxy._parse_mcp_request(self.rpc(nested))["params"]["arguments"], json.loads(nested))

    def test_other_tools_and_admin_keep_existing_behavior(self):
        for name in ("other", "astral_matrix_author_extra", "fs.astral_matrix_author"):
            with self.subTest(name=name):
                parsed = proxy._parse_mcp_request(self.rpc('{"x":1,"x":2}', name))
                self.assertEqual(parsed["params"]["arguments"]["x"], 2)
        # Registration/admin callers do not opt into the MCP-only guard.
        parsed = proxy._read_json_body(self.handler(self.rpc('{"x":1,"x":2}')))
        self.assertEqual(parsed["params"]["arguments"]["x"], 2)

    def test_malformed_json_stops_before_dispatch(self):
        for raw in (b'{"x":1,', b'{"x":"\\q"}', b'{} {}', b'\xff'):
            with self.subTest(raw=raw), mock.patch.object(proxy, "_handle_mcp_payload") as dispatch:
                handler = self.handler(raw)
                proxy.McpHandler.do_POST(handler)
                dispatch.assert_not_called()
                status, response = handler._respond_json.call_args.args
                self.assertEqual(status, 400)
                self.assertEqual(response["error"]["code"], -32700)

    def test_ambiguous_id_is_not_echoed(self):
        raw = b'{"id":1,"id":2,"method":"tools/call","params":{"name":"astral_matrix_save"}}'
        handler = self.handler(raw)
        with mock.patch.object(proxy, "_handle_mcp_payload") as dispatch:
            proxy.McpHandler.do_POST(handler)
            dispatch.assert_not_called()
        self.assertIsNone(handler._respond_json.call_args.args[1]["id"])


if __name__ == "__main__":
    unittest.main()
