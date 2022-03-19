/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <unistd.h>

#include "bootspec.h"
#include "bootspec-fundamental.h"
#include "conf-files.h"
#include "dirent-util.h"
#include "efi-loader.h"
#include "env-file.h"
#include "fd-util.h"
#include "fileio.h"
#include "find-esp.h"
#include "path-util.h"
#include "pe-header.h"
#include "sort-util.h"
#include "stat-util.h"
#include "strv.h"
#include "unaligned.h"

static void boot_entry_free(BootEntry *entry) {
        assert(entry);

        free(entry->id);
        free(entry->id_old);
        free(entry->path);
        free(entry->root);
        free(entry->title);
        free(entry->show_title);
        free(entry->sort_key);
        free(entry->version);
        free(entry->machine_id);
        free(entry->architecture);
        strv_free(entry->options);
        free(entry->kernel);
        free(entry->efi);
        strv_free(entry->initrd);
        free(entry->device_tree);
        strv_free(entry->device_tree_overlay);
}

static int boot_entry_load(
                const char *root,
                const char *path,
                BootEntry *entry) {

        _cleanup_(boot_entry_free) BootEntry tmp = {
                .type = BOOT_ENTRY_CONF,
        };

        _cleanup_fclose_ FILE *f = NULL;
        unsigned line = 1;
        char *c;
        int r;

        assert(root);
        assert(path);
        assert(entry);

        r = path_extract_filename(path, &tmp.id);
        if (r < 0)
                return log_error_errno(r, "Failed to extract file name from path '%s': %m", path);

        c = endswith_no_case(tmp.id, ".conf");
        if (!c)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid loader entry file suffix: %s", tmp.id);

        if (!efi_loader_entry_name_valid(tmp.id))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid loader entry name: %s", tmp.id);

        tmp.id_old = strndup(tmp.id, c - tmp.id);
        if (!tmp.id_old)
                return log_oom();

        tmp.path = strdup(path);
        if (!tmp.path)
                return log_oom();

        tmp.root = strdup(root);
        if (!tmp.root)
                return log_oom();

        f = fopen(path, "re");
        if (!f)
                return log_error_errno(errno, "Failed to open \"%s\": %m", path);

        for (;;) {
                _cleanup_free_ char *buf = NULL, *field = NULL;
                const char *p;

                r = read_line(f, LONG_LINE_MAX, &buf);
                if (r == 0)
                        break;
                if (r == -ENOBUFS)
                        return log_error_errno(r, "%s:%u: Line too long", path, line);
                if (r < 0)
                        return log_error_errno(r, "%s:%u: Error while reading: %m", path, line);

                line++;

                if (IN_SET(*strstrip(buf), '#', '\0'))
                        continue;

                p = buf;
                r = extract_first_word(&p, &field, " \t", 0);
                if (r < 0) {
                        log_error_errno(r, "Failed to parse config file %s line %u: %m", path, line);
                        continue;
                }
                if (r == 0) {
                        log_warning("%s:%u: Bad syntax", path, line);
                        continue;
                }

                if (streq(field, "title"))
                        r = free_and_strdup(&tmp.title, p);
                else if (streq(field, "sort-key"))
                        r = free_and_strdup(&tmp.sort_key, p);
                else if (streq(field, "version"))
                        r = free_and_strdup(&tmp.version, p);
                else if (streq(field, "machine-id"))
                        r = free_and_strdup(&tmp.machine_id, p);
                else if (streq(field, "architecture"))
                        r = free_and_strdup(&tmp.architecture, p);
                else if (streq(field, "options"))
                        r = strv_extend(&tmp.options, p);
                else if (streq(field, "linux"))
                        r = free_and_strdup(&tmp.kernel, p);
                else if (streq(field, "efi"))
                        r = free_and_strdup(&tmp.efi, p);
                else if (streq(field, "initrd"))
                        r = strv_extend(&tmp.initrd, p);
                else if (streq(field, "devicetree"))
                        r = free_and_strdup(&tmp.device_tree, p);
                else if (streq(field, "devicetree-overlay")) {
                        _cleanup_strv_free_ char **l = NULL;

                        l = strv_split(p, NULL);
                        if (!l)
                                return log_oom();

                        r = strv_extend_strv(&tmp.device_tree_overlay, l, false);
                } else {
                        log_notice("%s:%u: Unknown line \"%s\", ignoring.", path, line, field);
                        continue;
                }
                if (r < 0)
                        return log_error_errno(r, "%s:%u: Error while reading: %m", path, line);
        }

        *entry = tmp;
        tmp = (BootEntry) {};
        return 0;
}

void boot_config_free(BootConfig *config) {
        size_t i;

        assert(config);

        free(config->default_pattern);
        free(config->timeout);
        free(config->editor);
        free(config->auto_entries);
        free(config->auto_firmware);
        free(config->console_mode);
        free(config->random_seed_mode);
        free(config->beep);

        free(config->entry_oneshot);
        free(config->entry_default);
        free(config->entry_selected);

        for (i = 0; i < config->n_entries; i++)
                boot_entry_free(config->entries + i);
        free(config->entries);
}

static int boot_loader_read_conf(const char *path, BootConfig *config) {
        _cleanup_fclose_ FILE *f = NULL;
        unsigned line = 1;
        int r;

        assert(path);
        assert(config);

        f = fopen(path, "re");
        if (!f) {
                if (errno == ENOENT)
                        return 0;

                return log_error_errno(errno, "Failed to open \"%s\": %m", path);
        }

        for (;;) {
                _cleanup_free_ char *buf = NULL, *field = NULL;
                const char *p;

                r = read_line(f, LONG_LINE_MAX, &buf);
                if (r == 0)
                        break;
                if (r == -ENOBUFS)
                        return log_error_errno(r, "%s:%u: Line too long", path, line);
                if (r < 0)
                        return log_error_errno(r, "%s:%u: Error while reading: %m", path, line);

                line++;

                if (IN_SET(*strstrip(buf), '#', '\0'))
                        continue;

                p = buf;
                r = extract_first_word(&p, &field, " \t", 0);
                if (r < 0) {
                        log_error_errno(r, "Failed to parse config file %s line %u: %m", path, line);
                        continue;
                }
                if (r == 0) {
                        log_warning("%s:%u: Bad syntax", path, line);
                        continue;
                }

                if (streq(field, "default"))
                        r = free_and_strdup(&config->default_pattern, p);
                else if (streq(field, "timeout"))
                        r = free_and_strdup(&config->timeout, p);
                else if (streq(field, "editor"))
                        r = free_and_strdup(&config->editor, p);
                else if (streq(field, "auto-entries"))
                        r = free_and_strdup(&config->auto_entries, p);
                else if (streq(field, "auto-firmware"))
                        r = free_and_strdup(&config->auto_firmware, p);
                else if (streq(field, "console-mode"))
                        r = free_and_strdup(&config->console_mode, p);
                else if (streq(field, "random-seed-mode"))
                        r = free_and_strdup(&config->random_seed_mode, p);
                else if (streq(field, "beep"))
                        r = free_and_strdup(&config->beep, p);
                else {
                        log_notice("%s:%u: Unknown line \"%s\", ignoring.", path, line, field);
                        continue;
                }
                if (r < 0)
                        return log_error_errno(r, "%s:%u: Error while reading: %m", path, line);
        }

        return 1;
}

static int boot_entry_compare(const BootEntry *a, const BootEntry *b) {
        int r;

        assert(a);
        assert(b);

        r = CMP(!a->sort_key, !b->sort_key);
        if (r != 0)
                return r;
        if (a->sort_key && b->sort_key) {
                r = strcmp(a->sort_key, b->sort_key);
                if (r != 0)
                        return r;

                r = strcmp_ptr(a->machine_id, b->machine_id);
                if (r != 0)
                        return r;

                r = -strverscmp_improved(a->version, b->version);
                if (r != 0)
                        return r;
        }

        return strverscmp_improved(a->id, b->id);
}

static int boot_entries_find(
                const char *root,
                const char *dir,
                BootEntry **entries,
                size_t *n_entries) {

        _cleanup_strv_free_ char **files = NULL;
        char **f;
        int r;

        assert(root);
        assert(dir);
        assert(entries);
        assert(n_entries);

        r = conf_files_list(&files, ".conf", NULL, 0, dir);
        if (r < 0)
                return log_error_errno(r, "Failed to list files in \"%s\": %m", dir);

        STRV_FOREACH(f, files) {
                if (!GREEDY_REALLOC0(*entries, *n_entries + 1))
                        return log_oom();

                r = boot_entry_load(root, *f, *entries + *n_entries);
                if (r < 0)
                        continue;

                (*n_entries) ++;
        }

        return 0;
}

static int boot_entry_load_unified(
                const char *root,
                const char *path,
                const char *osrelease,
                const char *cmdline,
                BootEntry *ret) {

        _cleanup_free_ char *os_pretty_name = NULL, *os_image_id = NULL, *os_name = NULL, *os_id = NULL,
                *os_image_version = NULL, *os_version = NULL, *os_version_id = NULL, *os_build_id = NULL;
        _cleanup_(boot_entry_free) BootEntry tmp = {
                .type = BOOT_ENTRY_UNIFIED,
        };
        const char *k, *good_name, *good_version, *good_sort_key;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(root);
        assert(path);
        assert(osrelease);

        k = path_startswith(path, root);
        if (!k)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Path is not below root: %s", path);

        f = fmemopen_unlocked((void*) osrelease, strlen(osrelease), "r");
        if (!f)
                return log_error_errno(errno, "Failed to open os-release buffer: %m");

        r = parse_env_file(f, "os-release",
                           "PRETTY_NAME", &os_pretty_name,
                           "IMAGE_ID", &os_image_id,
                           "NAME", &os_name,
                           "ID", &os_id,
                           "IMAGE_VERSION", &os_image_version,
                           "VERSION", &os_version,
                           "VERSION_ID", &os_version_id,
                           "BUILD_ID", &os_build_id);
        if (r < 0)
                return log_error_errno(r, "Failed to parse os-release data from unified kernel image %s: %m", path);

        if (!bootspec_pick_name_version_sort_key(
                            os_pretty_name,
                            os_image_id,
                            os_name,
                            os_id,
                            os_image_version,
                            os_version,
                            os_version_id,
                            os_build_id,
                            &good_name,
                            &good_version,
                            &good_sort_key))
                return log_error_errno(SYNTHETIC_ERRNO(EBADMSG), "Missing fields in os-release data from unified kernel image %s, refusing.", path);

        r = path_extract_filename(path, &tmp.id);
        if (r < 0)
                return log_error_errno(r, "Failed to extract file name from '%s': %m", path);

        if (!efi_loader_entry_name_valid(tmp.id))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid loader entry name: %s", tmp.id);

        if (os_id && os_version_id) {
                tmp.id_old = strjoin(os_id, "-", os_version_id);
                if (!tmp.id_old)
                        return log_oom();
        }

        tmp.path = strdup(path);
        if (!tmp.path)
                return log_oom();

        tmp.root = strdup(root);
        if (!tmp.root)
                return log_oom();

        tmp.kernel = strdup(skip_leading_chars(k, "/"));
        if (!tmp.kernel)
                return log_oom();

        tmp.options = strv_new(skip_leading_chars(cmdline, WHITESPACE));
        if (!tmp.options)
                return log_oom();

        delete_trailing_chars(tmp.options[0], WHITESPACE);

        tmp.title = strdup(good_name);
        if (!tmp.title)
                return log_oom();

        tmp.sort_key = strdup(good_sort_key);
        if (!tmp.sort_key)
                return log_oom();

        tmp.version = strdup(good_version);
        if (!tmp.version)
                return log_oom();

        *ret = tmp;
        tmp = (BootEntry) {};
        return 0;
}

/* Maximum PE section we are willing to load (Note that sections we are not interested in may be larger, but
 * the ones we do care about and we are willing to load into memory have this size limit.) */
#define PE_SECTION_SIZE_MAX (4U*1024U*1024U)

static int find_sections(
                int fd,
                char **ret_osrelease,
                char **ret_cmdline) {

        _cleanup_free_ struct PeSectionHeader *sections = NULL;
        _cleanup_free_ char *osrelease = NULL, *cmdline = NULL;
        size_t i, n_sections;
        struct DosFileHeader dos;
        struct PeHeader pe;
        uint64_t start;
        ssize_t n;

        n = pread(fd, &dos, sizeof(dos), 0);
        if (n < 0)
                return log_error_errno(errno, "Failed read DOS header: %m");
        if (n != sizeof(dos))
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Short read while reading DOS header, refusing.");

        if (dos.Magic[0] != 'M' || dos.Magic[1] != 'Z')
                return log_error_errno(SYNTHETIC_ERRNO(EBADMSG), "DOS executable magic missing, refusing.");

        start = unaligned_read_le32(&dos.ExeHeader);
        n = pread(fd, &pe, sizeof(pe), start);
        if (n < 0)
                return log_error_errno(errno, "Failed to read PE header: %m");
        if (n != sizeof(pe))
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Short read while reading PE header, refusing.");

        if (pe.Magic[0] != 'P' || pe.Magic[1] != 'E' || pe.Magic[2] != 0 || pe.Magic[3] != 0)
                return log_error_errno(SYNTHETIC_ERRNO(EBADMSG), "PE executable magic missing, refusing.");

        n_sections = unaligned_read_le16(&pe.FileHeader.NumberOfSections);
        if (n_sections > 96)
                return log_error_errno(SYNTHETIC_ERRNO(EBADMSG), "PE header has too many sections, refusing.");

        sections = new(struct PeSectionHeader, n_sections);
        if (!sections)
                return log_oom();

        n = pread(fd, sections,
                  n_sections * sizeof(struct PeSectionHeader),
                  start + sizeof(pe) + unaligned_read_le16(&pe.FileHeader.SizeOfOptionalHeader));
        if (n < 0)
                return log_error_errno(errno, "Failed to read section data: %m");
        if ((size_t) n != n_sections * sizeof(struct PeSectionHeader))
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Short read while reading sections, refusing.");

        for (i = 0; i < n_sections; i++) {
                _cleanup_free_ char *k = NULL;
                uint32_t offset, size;
                char **b;

                if (strneq((char*) sections[i].Name, ".osrel", sizeof(sections[i].Name)))
                        b = &osrelease;
                else if (strneq((char*) sections[i].Name, ".cmdline", sizeof(sections[i].Name)))
                        b = &cmdline;
                else
                        continue;

                if (*b)
                        return log_error_errno(SYNTHETIC_ERRNO(EBADMSG), "Duplicate section %s, refusing.", sections[i].Name);

                offset = unaligned_read_le32(&sections[i].PointerToRawData);
                size = unaligned_read_le32(&sections[i].VirtualSize);

                if (size > PE_SECTION_SIZE_MAX)
                        return log_error_errno(SYNTHETIC_ERRNO(EBADMSG), "Section %s too large, refusing.", sections[i].Name);

                k = new(char, size+1);
                if (!k)
                        return log_oom();

                n = pread(fd, k, size, offset);
                if (n < 0)
                        return log_error_errno(errno, "Failed to read section payload: %m");
                if ((size_t) n != size)
                        return log_error_errno(SYNTHETIC_ERRNO(EIO), "Short read while reading section payload, refusing:");

                /* Allow one trailing NUL byte, but nothing more. */
                if (size > 0 && memchr(k, 0, size - 1))
                        return log_error_errno(SYNTHETIC_ERRNO(EBADMSG), "Section contains embedded NUL byte: %m");

                k[size] = 0;
                *b = TAKE_PTR(k);
        }

        if (!osrelease)
                return log_error_errno(SYNTHETIC_ERRNO(EBADMSG), "Image lacks .osrel section, refusing.");

        if (ret_osrelease)
                *ret_osrelease = TAKE_PTR(osrelease);
        if (ret_cmdline)
                *ret_cmdline = TAKE_PTR(cmdline);

        return 0;
}

static int boot_entries_find_unified(
                const char *root,
                const char *dir,
                BootEntry **entries,
                size_t *n_entries) {

        _cleanup_(closedirp) DIR *d = NULL;
        int r;

        assert(root);
        assert(dir);
        assert(entries);
        assert(n_entries);

        d = opendir(dir);
        if (!d) {
                if (errno == ENOENT)
                        return 0;

                return log_error_errno(errno, "Failed to open %s: %m", dir);
        }

        FOREACH_DIRENT(de, d, return log_error_errno(errno, "Failed to read %s: %m", dir)) {
                _cleanup_free_ char *j = NULL, *osrelease = NULL, *cmdline = NULL;
                _cleanup_close_ int fd = -1;

                if (!dirent_is_file(de))
                        continue;

                if (!endswith_no_case(de->d_name, ".efi"))
                        continue;

                if (!GREEDY_REALLOC0(*entries, *n_entries + 1))
                        return log_oom();

                fd = openat(dirfd(d), de->d_name, O_RDONLY|O_CLOEXEC|O_NONBLOCK);
                if (fd < 0) {
                        log_warning_errno(errno, "Failed to open %s/%s, ignoring: %m", dir, de->d_name);
                        continue;
                }

                r = fd_verify_regular(fd);
                if (r < 0) {
                        log_warning_errno(r, "File %s/%s is not regular, ignoring: %m", dir, de->d_name);
                        continue;
                }

                r = find_sections(fd, &osrelease, &cmdline);
                if (r < 0)
                        continue;

                j = path_join(dir, de->d_name);
                if (!j)
                        return log_oom();

                r = boot_entry_load_unified(root, j, osrelease, cmdline, *entries + *n_entries);
                if (r < 0)
                        continue;

                (*n_entries) ++;
        }

        return 0;
}

static bool find_nonunique(const BootEntry *entries, size_t n_entries, bool arr[]) {
        bool non_unique = false;

        assert(entries || n_entries == 0);
        assert(arr || n_entries == 0);

        for (size_t i = 0; i < n_entries; i++)
                arr[i] = false;

        for (size_t i = 0; i < n_entries; i++)
                for (size_t j = 0; j < n_entries; j++)
                        if (i != j && streq(boot_entry_title(entries + i),
                                            boot_entry_title(entries + j)))
                                non_unique = arr[i] = arr[j] = true;

        return non_unique;
}

static int boot_entries_uniquify(BootEntry *entries, size_t n_entries) {
        _cleanup_free_ bool *arr = NULL;
        char *s;

        assert(entries || n_entries == 0);

        if (n_entries == 0)
                return 0;

        arr = new(bool, n_entries);
        if (!arr)
                return -ENOMEM;

        /* Find _all_ non-unique titles */
        if (!find_nonunique(entries, n_entries, arr))
                return 0;

        /* Add version to non-unique titles */
        for (size_t i = 0; i < n_entries; i++)
                if (arr[i] && entries[i].version) {
                        if (asprintf(&s, "%s (%s)", boot_entry_title(entries + i), entries[i].version) < 0)
                                return -ENOMEM;

                        free_and_replace(entries[i].show_title, s);
                }

        if (!find_nonunique(entries, n_entries, arr))
                return 0;

        /* Add machine-id to non-unique titles */
        for (size_t i = 0; i < n_entries; i++)
                if (arr[i] && entries[i].machine_id) {
                        if (asprintf(&s, "%s (%s)", boot_entry_title(entries + i), entries[i].machine_id) < 0)
                                return -ENOMEM;

                        free_and_replace(entries[i].show_title, s);
                }

        if (!find_nonunique(entries, n_entries, arr))
                return 0;

        /* Add file name to non-unique titles */
        for (size_t i = 0; i < n_entries; i++)
                if (arr[i]) {
                        if (asprintf(&s, "%s (%s)", boot_entry_title(entries + i), entries[i].id) < 0)
                                return -ENOMEM;

                        free_and_replace(entries[i].show_title, s);
                }

        return 0;
}

static int boot_config_find(const BootConfig *config, const char *id) {
        assert(config);

        if (!id)
                return -1;

        for (size_t i = 0; i < config->n_entries; i++)
                if (fnmatch(id, config->entries[i].id, FNM_CASEFOLD) == 0)
                        return i;

        return -1;
}

static int boot_entries_select_default(const BootConfig *config) {
        int i;

        assert(config);
        assert(config->entries || config->n_entries == 0);

        if (config->n_entries == 0) {
                log_debug("Found no default boot entry :(");
                return -1; /* -1 means "no default" */
        }

        if (config->entry_oneshot) {
                i = boot_config_find(config, config->entry_oneshot);
                if (i >= 0) {
                        log_debug("Found default: id \"%s\" is matched by LoaderEntryOneShot",
                                  config->entries[i].id);
                        return i;
                }
        }

        if (config->entry_default) {
                i = boot_config_find(config, config->entry_default);
                if (i >= 0) {
                        log_debug("Found default: id \"%s\" is matched by LoaderEntryDefault",
                                  config->entries[i].id);
                        return i;
                }
        }

        if (config->default_pattern) {
                i = boot_config_find(config, config->default_pattern);
                if (i >= 0) {
                        log_debug("Found default: id \"%s\" is matched by pattern \"%s\"",
                                  config->entries[i].id, config->default_pattern);
                        return i;
                }
        }

        log_debug("Found default: first entry \"%s\"", config->entries[0].id);
        return 0;
}

static int boot_entries_select_selected(const BootConfig *config) {
        assert(config);
        assert(config->entries || config->n_entries == 0);

        if (!config->entry_selected || config->n_entries == 0)
                return -1;

        return boot_config_find(config, config->entry_selected);
}

static int boot_load_efi_entry_pointers(BootConfig *config) {
        int r;

        assert(config);

        if (!is_efi_boot())
                return 0;

        /* Loads the three "pointers" to boot loader entries from their EFI variables */

        r = efi_get_variable_string(EFI_LOADER_VARIABLE(LoaderEntryOneShot), &config->entry_oneshot);
        if (r < 0 && !IN_SET(r, -ENOENT, -ENODATA)) {
                log_warning_errno(r, "Failed to read EFI variable \"LoaderEntryOneShot\": %m");
                if (r == -ENOMEM)
                        return r;
        }

        r = efi_get_variable_string(EFI_LOADER_VARIABLE(LoaderEntryDefault), &config->entry_default);
        if (r < 0 && !IN_SET(r, -ENOENT, -ENODATA)) {
                log_warning_errno(r, "Failed to read EFI variable \"LoaderEntryDefault\": %m");
                if (r == -ENOMEM)
                        return r;
        }

        r = efi_get_variable_string(EFI_LOADER_VARIABLE(LoaderEntrySelected), &config->entry_selected);
        if (r < 0 && !IN_SET(r, -ENOENT, -ENODATA)) {
                log_warning_errno(r, "Failed to read EFI variable \"LoaderEntrySelected\": %m");
                if (r == -ENOMEM)
                        return r;
        }

        return 1;
}

int boot_entries_load_config(
                const char *esp_path,
                const char *xbootldr_path,
                BootConfig *config) {

        const char *p;
        int r;

        assert(config);

        if (esp_path) {
                p = strjoina(esp_path, "/loader/loader.conf");
                r = boot_loader_read_conf(p, config);
                if (r < 0)
                        return r;

                p = strjoina(esp_path, "/loader/entries");
                r = boot_entries_find(esp_path, p, &config->entries, &config->n_entries);
                if (r < 0)
                        return r;

                p = strjoina(esp_path, "/EFI/Linux/");
                r = boot_entries_find_unified(esp_path, p, &config->entries, &config->n_entries);
                if (r < 0)
                        return r;
        }

        if (xbootldr_path) {
                p = strjoina(xbootldr_path, "/loader/entries");
                r = boot_entries_find(xbootldr_path, p, &config->entries, &config->n_entries);
                if (r < 0)
                        return r;

                p = strjoina(xbootldr_path, "/EFI/Linux/");
                r = boot_entries_find_unified(xbootldr_path, p, &config->entries, &config->n_entries);
                if (r < 0)
                        return r;
        }

        typesafe_qsort(config->entries, config->n_entries, boot_entry_compare);

        r = boot_entries_uniquify(config->entries, config->n_entries);
        if (r < 0)
                return log_error_errno(r, "Failed to uniquify boot entries: %m");

        r = boot_load_efi_entry_pointers(config);
        if (r < 0)
                return r;

        config->default_entry = boot_entries_select_default(config);
        config->selected_entry = boot_entries_select_selected(config);

        return 0;
}

int boot_entries_load_config_auto(
                const char *override_esp_path,
                const char *override_xbootldr_path,
                BootConfig *config) {

        _cleanup_free_ char *esp_where = NULL, *xbootldr_where = NULL;
        dev_t esp_devid = 0, xbootldr_devid = 0;
        int r;

        assert(config);

        /* This function is similar to boot_entries_load_config(), however we automatically search for the
         * ESP and the XBOOTLDR partition unless it is explicitly specified. Also, if the user did not pass
         * an ESP or XBOOTLDR path directly, let's see if /run/boot-loader-entries/ exists. If so, let's
         * read data from there, as if it was an ESP (i.e. loading both entries and loader.conf data from
         * it). This allows other boot loaders to pass boot loader entry information to our tools if they
         * want to. */

        if (!override_esp_path && !override_xbootldr_path) {
                if (access("/run/boot-loader-entries/", F_OK) >= 0)
                        return boot_entries_load_config("/run/boot-loader-entries/", NULL, config);

                if (errno != ENOENT)
                        return log_error_errno(errno,
                                               "Failed to determine whether /run/boot-loader-entries/ exists: %m");
        }

        r = find_esp_and_warn(override_esp_path, /* unprivileged_mode= */ false, &esp_where, NULL, NULL, NULL, NULL, &esp_devid);
        if (r < 0) /* we don't log about ENOKEY here, but propagate it, leaving it to the caller to log */
                return r;

        r = find_xbootldr_and_warn(override_xbootldr_path, /* unprivileged_mode= */ false, &xbootldr_where, NULL, &xbootldr_devid);
        if (r < 0 && r != -ENOKEY)
                return r; /* It's fine if the XBOOTLDR partition doesn't exist, hence we ignore ENOKEY here */

        /* If both paths actually refer to the same inode, suppress the xbootldr path */
        if (esp_where && xbootldr_where && devid_set_and_equal(esp_devid, xbootldr_devid))
                xbootldr_where = mfree(xbootldr_where);

        return boot_entries_load_config(esp_where, xbootldr_where, config);
}

int boot_entries_augment_from_loader(
                BootConfig *config,
                char **found_by_loader,
                bool only_auto) {

        static const char *const title_table[] = {
                /* Pretty names for a few well-known automatically discovered entries. */
                "auto-osx",                      "macOS",
                "auto-windows",                  "Windows Boot Manager",
                "auto-efi-shell",                "EFI Shell",
                "auto-efi-default",              "EFI Default Loader",
                "auto-reboot-to-firmware-setup", "Reboot Into Firmware Interface",
        };

        char **i;

        assert(config);

        /* Let's add the entries discovered by the boot loader to the end of our list, unless they are
         * already included there. */

        STRV_FOREACH(i, found_by_loader) {
                BootEntry *existing;
                _cleanup_free_ char *c = NULL, *t = NULL, *p = NULL;
                char **a, **b;

                existing = boot_config_find_entry(config, *i);
                if (existing) {
                        existing->reported_by_loader = true;
                        continue;
                }

                if (only_auto && !startswith(*i, "auto-"))
                        continue;

                c = strdup(*i);
                if (!c)
                        return log_oom();

                STRV_FOREACH_PAIR(a, b, (char**) title_table)
                        if (streq(*a, *i)) {
                                t = strdup(*b);
                                if (!t)
                                        return log_oom();
                                break;
                        }

                p = strdup(EFIVAR_PATH(EFI_LOADER_VARIABLE(LoaderEntries)));
                if (!p)
                        return log_oom();

                if (!GREEDY_REALLOC0(config->entries, config->n_entries + 1))
                        return log_oom();

                config->entries[config->n_entries++] = (BootEntry) {
                        .type = startswith(*i, "auto-") ? BOOT_ENTRY_LOADER_AUTO : BOOT_ENTRY_LOADER,
                        .id = TAKE_PTR(c),
                        .title = TAKE_PTR(t),
                        .path = TAKE_PTR(p),
                        .reported_by_loader = true,
                };
        }

        return 0;
}
