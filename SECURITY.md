# Security Policy

## Architecture

Claireon runs an **unauthenticated HTTP server intended for localhost-only use** inside the Unreal Editor. This is by design, it enables AI assistants and local tools to interact with the editor without manual token exchange. Note that "localhost-only" is enforced in software, not by the socket bind. See the threat model below before deploying.

### Threat Model

- **Loopback access is enforced at the application layer, not by the socket bind**: The editor's HTTP server is built on Unreal's `FHttpServerModule`, which does not expose a bind-address option and listens on all interfaces. Loopback-only access is therefore enforced in software: requests carrying a non-localhost `Origin` or `Host` header are rejected as a best-effort defense against DNS rebinding. These header checks can be defeated by a caller that forges or omits the headers, so they are not a substitute for a host firewall. **Treat a firewall that blocks inbound connections to the MCP ports as the actual network boundary.**
- **The optional proxy binds loopback explicitly**: When enabled, the MCP proxy (`claireon_proxy.py`) binds `127.0.0.1` for its registration port (`43017`) and the per-worktree MCP ports, and gates editor ingress with a per-session bearer token. The proxy's own MCP endpoint remains unauthenticated on localhost.
- **Sandbox escape is by design**: The `python_execute` tool grants full access to the `unreal` Python module, the local filesystem, and the network. This is intentional, it enables the same workflows a developer would perform manually in the editor's Python console.
- **No authentication in the default mode**: In direct-connect mode (the default), any process that can reach the port can call the MCP server with no credentials; the header check above is the only application-layer control. (Proxy mode adds a per-session bearer token between the proxy and the editor.) This matches the trust model of the Unreal Editor itself: anyone who can run code on your machine already has full access. It is not a network access control.
- **Audit logging**: All Python executions are recorded to disk (`Saved/MCP/PythonAuditLog/`). This provides forensic traceability but does not prevent execution.

### What This Means

- **Do not expose the MCP port to the network.** Do not use port forwarding, reverse proxies, or tunnels to make the server accessible from other machines.
- **Do not run the MCP server on shared or multi-tenant systems** where other users could reach localhost.
- **Python execution has no sandbox.** Scripts can read/write files, make network requests, and modify the editor state.
- **Console commands are unrestricted.** The `console_execute` tool passes commands directly to the engine, equivalent to the in-editor console.
- **The built-in REPL stores your Anthropic API key** in the per-project editor settings file (`Saved/Config/`). Do not share or commit this file.

## Supported Versions

| Version | Supported          |
|---------|--------------------|
| 2.0.x   | Yes                |
| < 2.0   | No (pre-release)   |

## Reporting a Vulnerability

If you discover a security issue, please report it responsibly. **Do not open public issues for security vulnerabilities.**

### How to Report

1. **Email**: Send details to **security@believer.gg** with the subject line `[Claireon Security]`
2. **GitHub**: If private vulnerability reporting is enabled on this repository, you can use the "Report a vulnerability" button on the [Security tab](https://github.com/believer-oss/Claireon/security)

### What to Include

- Description of the vulnerability
- Steps to reproduce
- Impact assessment (what an attacker could achieve)
- Affected version(s)
- Suggested fix, if you have one

### Response Timeline

| Stage                  | Target        |
|------------------------|---------------|
| Acknowledgment         | 48 hours      |
| Initial triage         | 5 business days |
| Fix or mitigation plan | 30 days       |
| Public disclosure       | After fix ships, coordinated with reporter |

We will credit reporters in the release notes unless they prefer to remain anonymous.

### Coordinated Disclosure

We ask that you give us reasonable time to address the issue before public disclosure. We commit to:

- Keeping you informed of progress
- Crediting your contribution (unless you opt out)
- Publishing a security advisory once a fix is available

## Scope

### In Scope

- Remote code execution or access bypass that allows non-localhost callers to reach the MCP server
- Vulnerabilities in the HTTP server or JSON-RPC dispatch that could cause crashes, memory corruption, or information disclosure
- Path traversal or injection in tool parameters that achieves unintended effects beyond what the tool documents
- Information disclosure of sensitive data (API keys, credentials) through tool responses or logs
- Header validation bypasses (Origin/Host checks)

### Out of Scope

- Python script execution doing "dangerous" things (filesystem writes, network access, process spawning). This is by design; see Architecture above.
- Local privilege escalation. The editor already runs with the user's full privileges.
- Denial of service against the local editor process. Restarting the editor is the mitigation.
- Social engineering attacks that trick a user into running malicious Python code. This is equivalent to running any untrusted code.
- Vulnerabilities in Unreal Engine itself. Report those to [Epic Games](https://www.unrealengine.com/en-US/security).

## Security Hardening Recommendations

If you are deploying Claireon in an environment with heightened security requirements:

1. **Firewall the MCP ports.** Block all inbound (non-loopback) connections to the proxy registration port (`43017`) and the per-worktree MCP ports (the ephemeral range `49152-65535`; each worktree's live port is recorded in `Saved/Claireon/MCPServer.json`). Because the editor's HTTP listener binds all interfaces, this firewall rule is the real network boundary, not the header checks.
2. **Do not commit editor config files** (`Saved/Config/`) to version control. They may contain your Anthropic API key.
3. **Review the Python audit log** periodically at `Saved/MCP/PythonAuditLog/` for unexpected executions.
4. **Use a dedicated API key** for the built-in REPL with appropriate spend limits.
