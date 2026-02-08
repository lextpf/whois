#pragma once

/// Major version number.
#define WHOIS_VERSION_MAJOR 0
/// Minor version number.
#define WHOIS_VERSION_MINOR 1
/// Patch version number.
#define WHOIS_VERSION_PATCH 0
/// Release version number.
#define WHOIS_VERSION_RELEASE 0

/**
 * Stringify version components.
 *
 * @param major Major version.
 * @param minor Minor version.
 * @param patch Patch version.
 * @param release Release version.
 *
 * @see WHOIS_VERSION
 */
#define WHOIS_VERSION_STRINGIFY(major, minor, patch, release) \
	#major "." #minor "." #patch "." #release

/// Complete version string (e.g., "0.1.0.0").
#define WHOIS_VERSION \
	WHOIS_VERSION_STRINGIFY( \
		WHOIS_VERSION_MAJOR, \
		WHOIS_VERSION_MINOR, \
		WHOIS_VERSION_PATCH, \
		WHOIS_VERSION_RELEASE)

