# Brand: RagoShell

Decided 2026-10-31. The product, the company's software brand and the domain are **RagoShell** /
`ragoshell.com.tr`. "Rago" is the owner's; "Shell" names the product's centre, the embedded MongoDB
shell. No "Mongo", "DB" or "Studio" anywhere in the brand (MongoDB Inc. trademark standards; "Studio 3T"
is a registered mark in the same class).

## Spelling

| Context | Form |
|---|---|
| Product name, titles, window title, About, store listings | **RagoShell** — CamelCase, capital S, one word |
| In a sentence, also at the start | RagoShell (never "Ragoshell", never "Rago Shell", never "RAGOSHELL") |
| Domain, handles, package and binary names | `ragoshell` — all lower case: `ragoshell.com.tr`, `ragoshell.exe`, `ragoshell-mcp`, `github.com/ragipunal/ragoshell` |
| Bundle / package id | `tr.com.ragoshell.app` |
| Settings folder | `~/.ragoshell/` |
| Environment variables | `RAGOSHELL_CONFIG_DIR`, `RAGOSHELL_KEYSTORE` |
| Internal code prefix | `RAGO_`, `rago-core`, `rago_unit_tests`, `Rago*` QML components — unchanged, not user-visible |
| Repositories | `ragoshell`, `ragoshell-site`, `ragoshell-api`, `ragoshell-infra`, `ragoshell-bridge` |
| Services | "RagoShell Cloud" (optional cloud services), "RagoShell Bridge" (the WebSocket bridge) |
| Abbreviation | none; do not write "RS" |

The brand is **never translated or transliterated**: Arabic, Russian, Japanese and Chinese text keeps the
Latin "RagoShell". In the UI it is inserted with a placeholder (`qsTr("Welcome to %1").arg(Brand.productName)`),
so it is not part of any translatable string.

## Describing the product

"RagoShell is a MongoDB client" is fine: naming MongoDB to say what the tool connects to is nominative
use. Required notice on the web site, in About and in store listings:

> MongoDB is a registered trademark of MongoDB, Inc. RagoShell is not affiliated with or endorsed by
> MongoDB, Inc.

Do not use MongoDB's leaf logo or its green anywhere.

## Logo

The current flower icon can stay. The wordmark is "RagoShell" set in one typeface, "Rago" and "Shell" in
two tones. Do **not** use sea-shell imagery, in particular nothing resembling the yellow-and-red scallop
of the Shell oil company; "shell" here is the computing term, and the artwork must say so (a terminal
cursor `>_`, a prompt, the existing plant motif).

## One source in code

`cmake/RagoShellDefaults.cmake` holds `RAGO_PRODUCT_NAME`, `RAGO_PRODUCT_ID`, `RAGO_BUNDLE_ID`,
`RAGO_DOMAIN`, `RAGO_PRODUCT_URL`, `RAGO_DONATE_URL`; the generated `BrandInfo.h` and the QML `Brand` properties expose them.
No other file spells the name.
