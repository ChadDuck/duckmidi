/* Update checks: version comparison and release asset choice, from generated release JSON. No
   network access. */
#include "update.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    assert(dm_version_compare("0.11.0", "0.10.2") > 0);
    assert(dm_version_compare("v0.10.2", "0.10.2") == 0);
    assert(dm_version_compare("0.10.10", "0.10.9") > 0);
    assert(dm_version_compare("1.0", "0.99.99") > 0);
    assert(dm_version_compare("0.9.0", "0.10.0") < 0);

    const char *release =
        "{\"tag_name\": \"v0.12.0\", \"name\": \"duckmidi 0.12.0\", \"assets\": ["
        "{\"name\": \"notes.txt\", \"browser_download_url\": "
        "\"https://github.com/ChadDuck/duckmidi/releases/download/v0.12.0/notes.txt\"},"
        "{\"name\": \"duckmidi-v0.12.0-Core.zip\", \"browser_download_url\": "
        "\"https://github.com/ChadDuck/duckmidi/releases/download/v0.12.0/duckmidi-v0.12.0-Core.zip\"},"
        "{\"name\": \"duckmidi-v0.12.0-Full.zip\", \"browser_download_url\": "
        "\"https://github.com/ChadDuck/duckmidi/releases/download/v0.12.0/duckmidi-v0.12.0-Full.zip\"}]}";
    char version[32], url[1024];
    assert(dm_release_parse(release, true, version, sizeof(version), url, sizeof(url)));
    assert(!strcmp(version, "0.12.0") && strstr(url, "-Full.zip"));
    assert(dm_release_parse(release, false, version, sizeof(version), url, sizeof(url)));
    assert(strstr(url, "-Core.zip"));

    /* A single ZIP serves both builds; a release without one has nothing to install. */
    const char *single = "{\"tag_name\": \"0.12.1\", \"assets\": [{\"browser_download_url\": "
                         "\"https://example.invalid/duckmidi.zip\"}]}";
    assert(dm_release_parse(single, true, version, sizeof(version), url, sizeof(url)) &&
           !strcmp(version, "0.12.1") && strstr(url, "duckmidi.zip"));
    assert(!dm_release_parse("{\"tag_name\": \"v1.0.0\", \"assets\": []}", true, version,
                             sizeof(version), url, sizeof(url)));
    assert(!dm_release_parse("{}", true, version, sizeof(version), url, sizeof(url)));
    puts("PASS update version comparison and release asset choice");
    return 0;
}
