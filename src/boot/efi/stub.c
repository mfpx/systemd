/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <efi.h>
#include <efilib.h>

#include "cpio.h"
#include "devicetree.h"
#include "disk.h"
#include "graphics.h"
#include "linux.h"
#include "measure.h"
#include "pe.h"
#include "secure-boot.h"
#include "splash.h"
#include "tpm-pcr.h"
#include "util.h"

/* magic string to find in the binary image */
_used_ _section_(".sdmagic") static const char magic[] = "#### LoaderInfo: systemd-stub " GIT_VERSION " ####";

static EFI_STATUS combine_initrd(
                EFI_PHYSICAL_ADDRESS initrd_base, UINTN initrd_size,
                const void *credential_initrd, UINTN credential_initrd_size,
                const void *global_credential_initrd, UINTN global_credential_initrd_size,
                const void *sysext_initrd, UINTN sysext_initrd_size,
                EFI_PHYSICAL_ADDRESS *ret_initrd_base, UINTN *ret_initrd_size) {

        EFI_PHYSICAL_ADDRESS base = UINT32_MAX; /* allocate an area below the 32bit boundary for this */
        EFI_STATUS err;
        uint8_t *p;
        UINTN n;

        assert(ret_initrd_base);
        assert(ret_initrd_size);

        /* Combines four initrds into one, by simple concatenation in memory */

        n = ALIGN4(initrd_size); /* main initrd might not be padded yet */
        if (credential_initrd) {
                if (n > UINTN_MAX - credential_initrd_size)
                        return EFI_OUT_OF_RESOURCES;

                n += credential_initrd_size;
        }
        if (global_credential_initrd) {
                if (n > UINTN_MAX - global_credential_initrd_size)
                        return EFI_OUT_OF_RESOURCES;

                n += global_credential_initrd_size;
        }
        if (sysext_initrd) {
                if (n > UINTN_MAX - sysext_initrd_size)
                        return EFI_OUT_OF_RESOURCES;

                n += sysext_initrd_size;
        }

        err = BS->AllocatePages(
                        AllocateMaxAddress,
                        EfiLoaderData,
                        EFI_SIZE_TO_PAGES(n),
                        &base);
        if (err != EFI_SUCCESS)
                return log_error_status_stall(err, L"Failed to allocate space for combined initrd: %r", err);

        p = PHYSICAL_ADDRESS_TO_POINTER(base);
        if (initrd_base != 0) {
                UINTN pad;

                /* Order matters, the real initrd must come first, since it might include microcode updates
                 * which the kernel only looks for in the first cpio archive */
                memcpy(p, PHYSICAL_ADDRESS_TO_POINTER(initrd_base), initrd_size);
                p += initrd_size;

                pad = ALIGN4(initrd_size) - initrd_size;
                if (pad > 0)  {
                        memset(p, 0, pad);
                        p += pad;
                }
        }

        if (credential_initrd) {
                memcpy(p, credential_initrd, credential_initrd_size);
                p += credential_initrd_size;
        }

        if (global_credential_initrd) {
                memcpy(p, global_credential_initrd, global_credential_initrd_size);
                p += global_credential_initrd_size;
        }

        if (sysext_initrd) {
                memcpy(p, sysext_initrd, sysext_initrd_size);
                p += sysext_initrd_size;
        }

        assert((uint8_t*) PHYSICAL_ADDRESS_TO_POINTER(base) + n == p);

        *ret_initrd_base = base;
        *ret_initrd_size = n;

        return EFI_SUCCESS;
}

static void export_variables(EFI_LOADED_IMAGE_PROTOCOL *loaded_image) {
        static const uint64_t stub_features =
                EFI_STUB_FEATURE_REPORT_BOOT_PARTITION |    /* We set LoaderDevicePartUUID */
                EFI_STUB_FEATURE_PICK_UP_CREDENTIALS |      /* We pick up credentials from the boot partition */
                EFI_STUB_FEATURE_PICK_UP_SYSEXTS |          /* We pick up system extensions from the boot partition */
                EFI_STUB_FEATURE_THREE_PCRS |               /* We can measure kernel image, parameters and sysext */
                0;

        char16_t uuid[37];

        assert(loaded_image);

        /* Export the device path this image is started from, if it's not set yet */
        if (efivar_get_raw(LOADER_GUID, L"LoaderDevicePartUUID", NULL, NULL) != EFI_SUCCESS)
                if (disk_get_part_uuid(loaded_image->DeviceHandle, uuid) == EFI_SUCCESS)
                        efivar_set(LOADER_GUID, L"LoaderDevicePartUUID", uuid, 0);

        /* If LoaderImageIdentifier is not set, assume the image with this stub was loaded directly from the
         * UEFI firmware without any boot loader, and hence set the LoaderImageIdentifier ourselves. Note
         * that some boot chain loaders neither set LoaderImageIdentifier nor make FilePath available to us,
         * in which case there's simple nothing to set for us. (The UEFI spec doesn't really say who's wrong
         * here, i.e. whether FilePath may be NULL or not, hence handle this gracefully and check if FilePath
         * is non-NULL explicitly.) */
        if (efivar_get_raw(LOADER_GUID, L"LoaderImageIdentifier", NULL, NULL) != EFI_SUCCESS &&
            loaded_image->FilePath) {
                _cleanup_free_ char16_t *s = NULL;

                s = DevicePathToStr(loaded_image->FilePath);
                if (s)
                        efivar_set(LOADER_GUID, L"LoaderImageIdentifier", s, 0);
                else
                        log_oom();
        }

        /* if LoaderFirmwareInfo is not set, let's set it */
        if (efivar_get_raw(LOADER_GUID, L"LoaderFirmwareInfo", NULL, NULL) != EFI_SUCCESS) {
                _cleanup_free_ char16_t *s = NULL;
                s = xpool_print(L"%s %u.%02u", ST->FirmwareVendor, ST->FirmwareRevision >> 16, ST->FirmwareRevision & 0xffff);
                efivar_set(LOADER_GUID, L"LoaderFirmwareInfo", s, 0);
        }

        /* ditto for LoaderFirmwareType */
        if (efivar_get_raw(LOADER_GUID, L"LoaderFirmwareType", NULL, NULL) != EFI_SUCCESS) {
                _cleanup_free_ char16_t *s = NULL;
                s = xpool_print(L"UEFI %u.%02u", ST->Hdr.Revision >> 16, ST->Hdr.Revision & 0xffff);
                efivar_set(LOADER_GUID, L"LoaderFirmwareType", s, 0);
        }


        /* add StubInfo (this is one is owned by the stub, hence we unconditionally override this with our
         * own data) */
        (void) efivar_set(LOADER_GUID, L"StubInfo", L"systemd-stub " GIT_VERSION, 0);

        (void) efivar_set_uint64_le(LOADER_GUID, L"StubFeatures", stub_features, 0);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *sys_table) {
        UINTN cmdline_len = 0, linux_size, initrd_size, dt_size;
        UINTN credential_initrd_size = 0, global_credential_initrd_size = 0, sysext_initrd_size = 0;
        _cleanup_free_ void *credential_initrd = NULL, *global_credential_initrd = NULL;
        _cleanup_free_ void *sysext_initrd = NULL;
        EFI_PHYSICAL_ADDRESS linux_base, initrd_base, dt_base;
        _cleanup_(devicetree_cleanup) struct devicetree_state dt_state = {};
        EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
        UINTN addrs[_UNIFIED_SECTION_MAX] = {}, szs[_UNIFIED_SECTION_MAX] = {};
        char *cmdline = NULL;
        _cleanup_free_ char *cmdline_owned = NULL;
        int sections_measured = -1, parameters_measured = -1;
        bool sysext_measured = false, m;
        EFI_STATUS err;

        InitializeLib(image, sys_table);
        debug_hook(L"systemd-stub");
        /* Uncomment the next line if you need to wait for debugger. */
        // debug_break();

        err = BS->OpenProtocol(
                        image,
                        &LoadedImageProtocol,
                        (void **)&loaded_image,
                        image,
                        NULL,
                        EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (err != EFI_SUCCESS)
                return log_error_status_stall(err, L"Error getting a LoadedImageProtocol handle: %r", err);

        err = pe_memory_locate_sections(loaded_image->ImageBase, unified_sections, addrs, szs);
        if (err != EFI_SUCCESS || szs[UNIFIED_SECTION_LINUX] == 0) {
                if (err == EFI_SUCCESS)
                        err = EFI_NOT_FOUND;
                return log_error_status_stall(err, L"Unable to locate embedded .linux section: %r", err);
        }

        /* Measure all "payload" of this PE image into a separate PCR (i.e. where nothing else is written
         * into so far), so that we have one PCR that we can nicely write policies against because it
         * contains all static data of this image, and thus can be easily be pre-calculated. */
        for (UnifiedSection section = 0; section < _UNIFIED_SECTION_MAX; section++) {
                m = false;

                if (szs[section] == 0) /* not found */
                        continue;

                /* First measure the name of the section */
                (void) tpm_log_event_ascii(
                                TPM_PCR_INDEX_KERNEL_IMAGE,
                                POINTER_TO_PHYSICAL_ADDRESS(unified_sections[section]),
                                strsize8(unified_sections[section]), /* including NUL byte */
                                unified_sections[section],
                                &m);

                sections_measured = sections_measured < 0 ? m : (sections_measured && m);

                /* Then measure the data of the section */
                (void) tpm_log_event_ascii(
                                TPM_PCR_INDEX_KERNEL_IMAGE,
                                POINTER_TO_PHYSICAL_ADDRESS(loaded_image->ImageBase) + addrs[section],
                                szs[section],
                                unified_sections[section],
                                &m);

                sections_measured = sections_measured < 0 ? m : (sections_measured && m);
        }

        /* After we are done, set an EFI variable that tells userspace this was done successfully, and encode
         * in it which PCR was used. */
        if (sections_measured > 0)
                (void) efivar_set_uint_string(LOADER_GUID, L"StubPcrKernelImage", TPM_PCR_INDEX_KERNEL_IMAGE, 0);

        /* Show splash screen as early as possible */
        graphics_splash((const uint8_t*) loaded_image->ImageBase + addrs[UNIFIED_SECTION_SPLASH], szs[UNIFIED_SECTION_SPLASH], NULL);

        if (szs[UNIFIED_SECTION_CMDLINE] > 0) {
                cmdline = (char *) loaded_image->ImageBase + addrs[UNIFIED_SECTION_CMDLINE];
                cmdline_len = szs[UNIFIED_SECTION_CMDLINE];
        }

        /* if we are not in secure boot mode, or none was provided, accept a custom command line and replace
         * the built-in one. We also do a superficial check whether first character of passed command line
         * is printable character (for compat with some Dell systems which fill in garbage?). */
        if ((!secure_boot_enabled() || cmdline_len == 0) &&
            loaded_image->LoadOptionsSize > 0 &&
            ((char16_t *) loaded_image->LoadOptions)[0] > 0x1F) {
                cmdline_len = (loaded_image->LoadOptionsSize / sizeof(char16_t)) * sizeof(char);
                cmdline = cmdline_owned = xnew(char, cmdline_len);

                for (UINTN i = 0; i < cmdline_len; i++) {
                        char16_t c = ((char16_t *) loaded_image->LoadOptions)[i];
                        cmdline[i] = c > 0x1F && c < 0x7F ? c : ' '; /* convert non-printable and non_ASCII characters to spaces. */
                }

                /* Let's measure the passed kernel command line into the TPM. Note that this possibly
                 * duplicates what we already did in the boot menu, if that was already used. However, since
                 * we want the boot menu to support an EFI binary, and want to this stub to be usable from
                 * any boot menu, let's measure things anyway. */
                m = false;
                (void) tpm_log_load_options(loaded_image->LoadOptions, &m);
                parameters_measured = m;
        }

        export_variables(loaded_image);

        if (pack_cpio(loaded_image,
                      NULL,
                      L".cred",
                      ".extra/credentials",
                      /* dir_mode= */ 0500,
                      /* access_mode= */ 0400,
                      /* tpm_pcr= */ (uint32_t[]) { TPM_PCR_INDEX_KERNEL_PARAMETERS, TPM_PCR_INDEX_KERNEL_PARAMETERS_COMPAT },
                      /* n_tpm_pcr= */ 2,
                      L"Credentials initrd",
                      &credential_initrd,
                      &credential_initrd_size,
                      &m) == EFI_SUCCESS)
                parameters_measured = parameters_measured < 0 ? m : (parameters_measured && m);

        if (pack_cpio(loaded_image,
                      L"\\loader\\credentials",
                      L".cred",
                      ".extra/global_credentials",
                      /* dir_mode= */ 0500,
                      /* access_mode= */ 0400,
                      /* tpm_pcr= */ (uint32_t[]) { TPM_PCR_INDEX_KERNEL_PARAMETERS, TPM_PCR_INDEX_KERNEL_PARAMETERS_COMPAT },
                      /* n_tpm_pcr= */ 2,
                      L"Global credentials initrd",
                      &global_credential_initrd,
                      &global_credential_initrd_size,
                      &m) == EFI_SUCCESS)
                parameters_measured = parameters_measured < 0 ? m : (parameters_measured && m);

        if (pack_cpio(loaded_image,
                      NULL,
                      L".raw",
                      ".extra/sysext",
                      /* dir_mode= */ 0555,
                      /* access_mode= */ 0444,
                      /* tpm_pcr= */ (uint32_t[]) { TPM_PCR_INDEX_INITRD_SYSEXTS },
                      /* n_tpm_pcr= */ 1,
                      L"System extension initrd",
                      &sysext_initrd,
                      &sysext_initrd_size,
                      &m) == EFI_SUCCESS)
                sysext_measured = m;

        if (parameters_measured > 0)
                (void) efivar_set_uint_string(LOADER_GUID, L"StubPcrKernelParameters", TPM_PCR_INDEX_KERNEL_PARAMETERS, 0);
        if (sysext_measured)
                (void) efivar_set_uint_string(LOADER_GUID, L"StubPcrInitRDSysExts", TPM_PCR_INDEX_INITRD_SYSEXTS, 0);

        linux_size = szs[UNIFIED_SECTION_LINUX];
        linux_base = POINTER_TO_PHYSICAL_ADDRESS(loaded_image->ImageBase) + addrs[UNIFIED_SECTION_LINUX];

        initrd_size = szs[UNIFIED_SECTION_INITRD];
        initrd_base = initrd_size != 0 ? POINTER_TO_PHYSICAL_ADDRESS(loaded_image->ImageBase) + addrs[UNIFIED_SECTION_INITRD] : 0;

        dt_size = szs[UNIFIED_SECTION_DTB];
        dt_base = dt_size != 0 ? POINTER_TO_PHYSICAL_ADDRESS(loaded_image->ImageBase) + addrs[UNIFIED_SECTION_DTB] : 0;

        if (credential_initrd || global_credential_initrd || sysext_initrd) {
                /* If we have generated initrds dynamically, let's combine them with the built-in initrd. */
                err = combine_initrd(
                                initrd_base, initrd_size,
                                credential_initrd, credential_initrd_size,
                                global_credential_initrd, global_credential_initrd_size,
                                sysext_initrd, sysext_initrd_size,
                                &initrd_base, &initrd_size);
                if (err != EFI_SUCCESS)
                        return err;

                /* Given these might be large let's free them explicitly, quickly. */
                credential_initrd = mfree(credential_initrd);
                global_credential_initrd = mfree(global_credential_initrd);
                sysext_initrd = mfree(sysext_initrd);
        }

        if (dt_size > 0) {
                err = devicetree_install_from_memory(
                                &dt_state, PHYSICAL_ADDRESS_TO_POINTER(dt_base), dt_size);
                if (err != EFI_SUCCESS)
                        log_error_stall(L"Error loading embedded devicetree: %r", err);
        }

        err = linux_exec(image, cmdline, cmdline_len,
                         PHYSICAL_ADDRESS_TO_POINTER(linux_base), linux_size,
                         PHYSICAL_ADDRESS_TO_POINTER(initrd_base), initrd_size);
        graphics_mode(false);
        return log_error_status_stall(err, L"Execution of embedded linux image failed: %r", err);
}
