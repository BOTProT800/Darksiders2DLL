#pragma once

// The validated DDS engine ships in both configurations. Legacy prototype
// switches must not silently select a different writer in a shipping build.
#if defined(DS2_RESOURCE_IDENTITY_PROBE) || defined(DS2_GENERAL_RESOLVER_PROTOTYPE) || defined(DS2_GENERAL_RESOLVER_WRITE_PROTOTYPE)
#error Obsolete prototype macros: use the shipping DDS engine and its INI configuration
#endif
#define DS2_RESOURCE_IDENTITY_ENABLED
#define DS2_GENERAL_DDS_ENABLED
#if !defined(DS2_TEST_OBSERVE_ONLY)
#define DS2_GENERAL_DDS_WRITE_ENABLED
#endif
