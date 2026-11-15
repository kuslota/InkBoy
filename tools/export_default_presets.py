#!/usr/bin/env python3
"""Write presets/<mode>/default.json from the compiled Defaults using the firmware's own encoder."""
import sys
from pathlib import Path

from host_build import CJSON, ROOT, run

CODE = r'''
#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include "preset_json.h"

static void write_default(const char *directory, display_mode_t mode, const char *name)
{
    char folder[512], path[600], json[PRESET_JSON_MAX];
    size_t length;
    snprintf(folder, sizeof(folder), "%s/%s", directory, name);
    mkdir(directory, 0777);
    mkdir(folder, 0777);
    snprintf(path, sizeof(path), "%s/default.json", folder);
    assert(preset_json_encode(display_preset_default(mode), json, sizeof(json), &length));
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(json, 1, length, f) == length);
    fclose(f);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    write_default(argv[1], DISPLAY_DITHERED, "dithered");
    write_default(argv[1], DISPLAY_GRAYSCALE, "grayscale");
    return 0;
}
'''


def export(directory):
    run(CODE, modules=['display_preset.c', 'gray_profile_store.c', 'preset_json.c'], vendor_sources=[CJSON],
        scenarios=[[str(directory)]])


if __name__ == '__main__':
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'presets'
    export(target)
    print(f'wrote {target}/dithered/default.json and {target}/grayscale/default.json')
