# Changelog

All notable changes to the `pkgi-psp` project will be documented in this file. This project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]()

## [v0.8.0](https://github.com/bucanero/pkgi-psp/releases/tag/v0.8.0) - 2023-10-01

First public release. In memory of Leon & Luna.

### Added

* Download and install PKG files to the PSP
* Support for loading multiple database files
  - Generic text database format support
  - Filter unsupported or missing URLs when loading a database
* Content categorization and filtering
* Support for HTTP, HTTPS, FTP, FTPS links with TLS v1.2
* Localization support (Finnish, French, German, Indonesian, Italian, Polish, Portuguese, Spanish, Turkish)
  - Language detection based on PSP settings
* Enter button detection (`cross`/`circle`)
