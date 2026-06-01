# Security Policy

## Supported Versions

OpusEdit is maintained from the `main` branch. Please test security fixes against
the latest tagged release when possible.

| Version | Supported |
| ------- | --------- |
| 2.x     | Yes       |
| 1.x     | No        |

## Reporting a Vulnerability

If you find a vulnerability, please open a private report using GitHub's
"Report a vulnerability" flow if it is available for this repository.

If private reporting is not available, open a public issue with a high-level
description and avoid posting exploit details, proof-of-concept payloads, or
sensitive files until the issue can be triaged.

Useful reports include:

- The affected version or commit.
- The operating system and compiler used.
- Steps to reproduce using harmless input.
- The expected behavior and actual behavior.
- Any sanitizer, debugger, or crash output.

## Security Focus Areas

OpusEdit is written in C and handles terminal input, file contents, and Git
object metadata. Security-sensitive areas include:

- Memory allocation and bounds checks.
- File open/save behavior.
- Terminal escape parsing.
- Git object decompression and parsing.
- Large or malformed input files.

## Disclosure Expectations

I aim to acknowledge valid reports as soon as possible, reproduce the issue,
prepare a fix on `main`, and publish a tagged release when the fix is ready.
