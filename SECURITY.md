# Security Policy

## Architecture

Claireon runs an **unauthenticated HTTP server on localhost** inside the Unreal Editor. This is by design — it enables AI assistants and local tools to interact with the editor without manual token exchange.

### Threat Model

- **Localhost-only**: The server binds to `127.0.0.1` and is not accessible from other machines on the network.
- **Sandbox escape is by design**: The `execute_python_script` tool grants full access to the `unreal` Python module, the local filesystem, and the network. This is intentional — it enables the same workflows a developer would perform manually in the editor's Python console.
- **No authentication**: Any process on the local machine can call the MCP server. This matches the trust model of the Unreal Editor itself (anyone who can run code on your machine already has full access).

### What This Means

- **Do not expose port 8017 (or your configured port) to the network.** Do not use port forwarding, reverse proxies, or tunnels to make the MCP server accessible from other machines.
- **Do not run the MCP server on shared/multi-tenant systems** where other users could reach localhost.
- **Python execution has no sandbox.** Scripts can read/write files, make network requests, and modify the editor state. The audit log (`editor.python.audit_log`) records all executions but does not prevent them.
- **Execution timeout is advisory only.** The `timeout_seconds` parameter on `execute_python_script` is not currently enforced at the process level.

## Supported Versions

| Version | Supported |
|---------|-----------|
| 2.0.x   | Yes       |
| < 2.0   | No        |

## Reporting a Vulnerability

If you discover a security issue, please report it responsibly:

1. **Preferred**: Open a [GitHub private security advisory](https://github.com/believer-oss/claireon/security/advisories/new)
2. **Alternative**: Email the maintainers (see GitHub profiles of org members)

**Please do not open public issues for security vulnerabilities.**

### What to Include

- Description of the vulnerability
- Steps to reproduce
- Impact assessment (what an attacker could do)
- Suggested fix (if you have one)

### What to Expect

- Acknowledgment within 48 hours
- Status update within 1 week
- Fix or mitigation plan within 30 days for confirmed issues

## Scope

### In Scope

- Authentication bypass that allows remote (non-localhost) access
- Vulnerabilities in the HTTP server or JSON-RPC dispatch that could cause crashes or memory corruption
- Path traversal or injection in tool parameters that bypasses intended behavior
- Information disclosure of sensitive data (API keys, credentials) through tool responses

### Out of Scope

- Python script execution doing "dangerous" things — this is by design (see Architecture above)
- Local privilege escalation — the editor already runs with the user's full privileges
- Denial of service against the local editor process — restarting the editor is the mitigation
