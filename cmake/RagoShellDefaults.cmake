# Single source for the product brand (docs/Brand.md).
# Display strings, installers and BrandInfo.h / QML Brand read these. Do not
# spell "RagoShell" elsewhere.

set(RAGO_PRODUCT_NAME "RagoShell")
set(RAGO_PRODUCT_ID "ragoshell")
set(RAGO_GITHUB_OWNER "ragipunal")
set(RAGO_BUNDLE_ID "tr.com.ragoshell.app")
set(RAGO_DOMAIN "ragoshell.com.tr")
set(RAGO_PRODUCT_URL "https://ragoshell.com.tr")

if(NOT DEFINED RAGO_SOURCE_REPO_URL)
    set(RAGO_SOURCE_REPO_URL "https://github.com/${RAGO_GITHUB_OWNER}/${RAGO_PRODUCT_ID}")
endif()
if(NOT DEFINED RAGO_DONATE_URL)
    set(RAGO_DONATE_URL "${RAGO_SOURCE_REPO_URL}/blob/main/DONATE.md")
endif()

if(NOT DEFINED RAGO_API_URL)
    set(RAGO_API_URL "https://api.${RAGO_DOMAIN}")
endif()
if(NOT DEFINED RAGO_PORTAL_URL)
    set(RAGO_PORTAL_URL "https://app.${RAGO_DOMAIN}")
endif()
if(NOT DEFINED RAGO_RELEASES_MANIFEST_URL)
    set(RAGO_RELEASES_MANIFEST_URL "https://dl.${RAGO_DOMAIN}/releases.json")
endif()
if(NOT DEFINED RAGO_PRIVACY_URL)
    set(RAGO_PRIVACY_URL "${RAGO_PRODUCT_URL}/legal/privacy/")
endif()
if(NOT DEFINED RAGO_TERMS_URL)
    set(RAGO_TERMS_URL "${RAGO_PRODUCT_URL}/legal/terms/")
endif()

# Minisign (Ed25519) public key (base64 RW...) used to verify releases.json and SHA256SUMS.txt
# (docs/ReleasePipeline.md).
if(NOT DEFINED RAGO_MINISIGN_PUBKEY)
    set(RAGO_MINISIGN_PUBKEY "RWQh7WakuhvFcr0CrX5kU8LP4jcCuJKt9JqZxOSWb8AEX9z1H4uERNej")
endif()

# Semicolon-separated ES256 (P-256) public key ring (<kid>:<base64-spki-der>;...) used to verify
# offline entitlement+jwt tokens from RagoShell Cloud (../ragoshell-api/docs/EntitlementTokens.md).
if(NOT DEFINED RAGO_ENTITLEMENT_PUBKEYS)
    set(RAGO_ENTITLEMENT_PUBKEYS "2026-11:MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEiVF7yAUAmnfVL0l3nN1yy+WdkRJO3F7nj9Ml7J+TeCsbDp73PFd9jplKjb71sa9yOucABWB98Q2LN3hEIXO6cg==")
endif()

if(RAGO_STORE_BUILD)
    set(RAGO_STORE_BUILD_BOOL "true")
else()
    set(RAGO_STORE_BUILD_BOOL "false")
endif()
