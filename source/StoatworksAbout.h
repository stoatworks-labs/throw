/*
 * Stoatworks Labs - About window data for Throw.
 *
 * A PROVISIONAL hand copy, adapted from slitscan's generated one, until the
 * plugin is registered in the website's projects.json and
 * stoatworks-backend/scripts/sync-about.py takes this file over. There is no
 * user guide yet, so `guide` is empty and the About block has no User guide
 * button (StoatworksAboutLinks.h leaves an empty link out).
 *
 * `version` here is a fallback read from this repo's own manifest. Anything
 * with a build step injects the real one at build time and overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "Throw";
    inline constexpr auto slug = "throw";
    inline constexpr auto hook = "Your clip, thrown by a projector onto a real surface, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/throw/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/throw";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
