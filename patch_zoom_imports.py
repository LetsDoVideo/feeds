"""
Patch Zoom SDK DLLs to use a renamed libcurl.dll, avoiding conflict with OBS's libcurl.dll.

Usage:
    pip install pefile
    python patch_zoom_imports.py <path-to-zoom-sdk-folder>

Example:
    python patch_zoom_imports.py ./zoom-sdk
"""

import pefile
import glob
import os
import sys

OLD_NAME = "libcurl.dll"
# New name MUST be same length or shorter than old name (11 chars).
# "zcurl.dll" = 9 chars, fits with null-padding.
NEW_NAME = "zcurl.dll"


def find_importers(sdk_dir):
    """Find all DLLs in sdk_dir that import OLD_NAME."""
    importers = []
    for dll_path in glob.glob(os.path.join(sdk_dir, "*.dll")):
        try:
            pe = pefile.PE(dll_path, fast_load=True)
            pe.parse_data_directories(
                directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]]
            )
            if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
                for entry in pe.DIRECTORY_ENTRY_IMPORT:
                    if entry.dll.decode("ascii").lower() == OLD_NAME.lower():
                        importers.append(dll_path)
                        break
            pe.close()
        except Exception as e:
            print(f"  Skipping {os.path.basename(dll_path)}: {e}")
    return importers


def patch_import_name(dll_path, old_name, new_name):
    """Patch a PE file's import table to replace old_name with new_name."""
    old_bytes = old_name.encode("ascii")
    new_bytes = new_name.encode("ascii")

    if len(new_bytes) > len(old_bytes):
        raise ValueError(
            f"New name '{new_name}' ({len(new_bytes)} chars) is longer than "
            f"old name '{old_name}' ({len(old_bytes)} chars). Cannot patch in-place."
        )

    pe = pefile.PE(dll_path)

    patched = False
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        if entry.dll.decode("ascii").lower() == old_name.lower():
            name_rva = entry.struct.Name
            name_offset = pe.get_offset_from_rva(name_rva)
            padded = new_bytes + b"\x00" * (len(old_bytes) - len(new_bytes))
            pe.set_bytes_at_offset(name_offset, padded)
            patched = True
            break

    if patched:
        pe.write(dll_path)
        print(f"  PATCHED: {os.path.basename(dll_path)} -> now imports '{new_name}'")
    else:
        print(f"  WARNING: {os.path.basename(dll_path)} - import entry not found")

    pe.close()
    return patched


def main():
    if len(sys.argv) < 2:
        print(f"Usage: python {sys.argv[0]} <path-to-zoom-sdk-folder>")
        sys.exit(1)

    sdk_dir = sys.argv[1]

    if not os.path.isdir(sdk_dir):
        print(f"ERROR: '{sdk_dir}' is not a directory")
        sys.exit(1)

    old_path = os.path.join(sdk_dir, OLD_NAME)
    new_path = os.path.join(sdk_dir, NEW_NAME)

    if not os.path.exists(old_path):
        if os.path.exists(new_path):
            print(f"'{NEW_NAME}' already exists (previously patched?). Scanning for remaining imports...")
        else:
            print(f"ERROR: '{OLD_NAME}' not found in '{sdk_dir}'")
            sys.exit(1)

    # Step 1: Find all DLLs that import libcurl.dll
    print(f"\nScanning for DLLs that import '{OLD_NAME}'...")
    importers = find_importers(sdk_dir)

    if not importers:
        print(f"  No DLLs import '{OLD_NAME}'. Nothing to patch.")
    else:
        print(f"  Found {len(importers)} DLL(s) to patch:")
        for p in importers:
            print(f"    - {os.path.basename(p)}")

        # Step 2: Patch each importer
        print(f"\nPatching import tables ('{OLD_NAME}' -> '{NEW_NAME}')...")
        for dll_path in importers:
            patch_import_name(dll_path, OLD_NAME, NEW_NAME)

    # Step 3: Rename the actual DLL file
    if os.path.exists(old_path):
        os.rename(old_path, new_path)
        print(f"\nRenamed: {OLD_NAME} -> {NEW_NAME}")

    print("\nDone. Zoom SDK DLLs are now isolated from OBS's libcurl.dll.")


if __name__ == "__main__":
    main()
