# Security Policy

## Architecture

Claireon runs an **unauthenticated HTTP server on localhost** inside the Unreal Editor. This is by design, it enables AI assistants and local tools to interact with the editor without manual token exchange.

### Threat Model

- **Localhost-only binding**: The server is intended to be accessible only from the local machine. Header validation rejects requests with non-localhost `Origin` or `Host` headers as a defense-in-depth measure against DNS rebinding attacks.
- **Sandbox escape is by design**: The `python_execute` tool grants full access to the `unreal` Python module, the local filesystem, and the network. This is intentional, it enables the same workflows a developer would perform manually in the editor's Python console.
- **No authentication**: Any process on the local machine can call the MCP server. This matches the trust model of the Unreal Editor itself (anyone who can run code on your machine already has full access).
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
2. **GitHub**: If private vulnerability reporting is enabled on this repository, you can use the "Report a vulnerability" button on the [Security tab](https://github.com/believer-oss/claireon/security)

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

1. **Firewall the MCP port** (default 8017). Block all inbound connections from non-loopback addresses.
2. **Do not commit editor config files** (`Saved/Config/`) to version control. They may contain your Anthropic API key.
3. **Review the Python audit log** periodically at `Saved/MCP/PythonAuditLog/` for unexpected executions.
4. **Use a dedicated API key** for the built-in REPL with appropriate spend limits.
