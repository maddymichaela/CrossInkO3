#pragma once

namespace ota_release_policy {

// Returns a positive value when latest is newer, a negative value when it is
// older, and zero when either value is invalid or both versions are equal.
int compareVersions(const char* latest, const char* current);

// Accepts both upstream firmware-<device>-v*.bin assets and this fork's
// CrossInk-AO3-v*-<device>.bin release assets.
bool matchesFirmwareAsset(const char* assetName, const char* deviceType);

}  // namespace ota_release_policy
