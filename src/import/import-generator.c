/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "sd-json.h"

#include "creds-util.h"
#include "discover-image.h"
#include "fd-util.h"
#include "fileio.h"
#include "generator.h"
#include "import-util.h"
#include "initrd-util.h"
#include "json-util.h"
#include "proc-cmdline.h"
#include "special.h"
#include "specifier.h"
#include "unit-name.h"
#include "web-util.h"

typedef struct Transfer {
        ImageClass class;
        ImportType type;
        char *local;
        char *remote;
        bool blockdev;
        sd_json_variant *json;
} Transfer;

static const char *arg_dest = NULL;
static char *arg_success_action = NULL;
static char *arg_failure_action = NULL;
static Transfer *arg_transfers = NULL;
static size_t arg_n_transfers = 0;

static void transfer_destroy_many(Transfer *transfers, size_t n) {
        FOREACH_ARRAY(t, transfers, n) {
                free(t->local);
                free(t->remote);
                sd_json_variant_unref(t->json);
        }

        free(transfers);
}

STATIC_DESTRUCTOR_REGISTER(arg_success_action, freep);
STATIC_DESTRUCTOR_REGISTER(arg_failure_action, freep);
STATIC_ARRAY_DESTRUCTOR_REGISTER(arg_transfers, arg_n_transfers, transfer_destroy_many);

static int parse_pull_expression(const char *v) {
        const char *p = v;
        int r;

        assert(v);

        _cleanup_free_ char *options = NULL;
        r = extract_first_word(&p, &options, ":", EXTRACT_DONT_COALESCE_SEPARATORS);
        if (r < 0)
                return log_error_errno(r, "Failed to extract option string from pull expression '%s': %m", v);
        if (r == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "No option string in pull expression '%s': %m", v);

        _cleanup_free_ char *local = NULL;
        r = extract_first_word(&p, &local, ":", EXTRACT_DONT_COALESCE_SEPARATORS);
        if (r < 0)
                return log_error_errno(r, "Failed to extract local name from pull expression '%s': %m", v);
        if (r == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "No local string in pull expression '%s': %m", v);

        if (!http_url_is_valid(p) && !file_url_is_valid(p))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Not a valid URL, refusing: %s", p);
        _cleanup_free_ char *remote = strdup(p);
        if (!remote)
                return log_oom();

        if (isempty(local))
                local = mfree(local);
        else if (!image_name_is_valid(local))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Not a valid image name, refusing: %s", local);

        ImportType type = _IMPORT_TYPE_INVALID;
        ImageClass class = _IMAGE_CLASS_INVALID;
        ImportVerify verify = IMPORT_VERIFY_SIGNATURE;
        bool ro = false, blockdev = false;

        const char *o = options;
        for (;;) {
                _cleanup_free_ char *opt = NULL;

                r = extract_first_word(&o, &opt, ",", EXTRACT_DONT_COALESCE_SEPARATORS);
                if (r < 0)
                        return log_error_errno(r, "Failed to extract option from pull option expression '%s': %m", options);
                if (r == 0)
                        break;

                const char *suffix;

                if (streq(opt, "ro"))
                        ro = true;
                else if (streq(opt, "rw"))
                        ro = false;
                else if (streq(opt, "blockdev"))
                        blockdev = true;
                else if ((suffix = startswith(opt, "verify="))) {

                        ImportVerify w = import_verify_from_string(suffix);

                        if (w < 0)
                                log_warning_errno(w, "Unknown verification mode, ignoring: %s", suffix);
                        else
                                verify = w;
                } else {
                        ImageClass c;

                        c = image_class_from_string(opt);
                        if (c < 0) {
                                ImportType t;

                                t = import_type_from_string(opt);
                                if (t < 0)
                                        log_warning_errno(c, "Unknown pull option, ignoring: %s", opt);
                                else
                                        type = t;
                        } else
                                class = c;
                }
        }

        if (type < 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "No image type (raw, tar) specified in pull expression, refusing: %s", v);
        if (class < 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "No image class (machine, portable, sysext, confext) specified in pull expression, refusing: %s", v);

        if (!local) {
                _cleanup_free_ char *c = NULL;
                r = import_url_last_component(remote, &c);
                if (r < 0)
                        return log_error_errno(r, "Failed to generate local name from URL '%s': %m", remote);

                switch (type) {

                case IMPORT_RAW:
                        r = raw_strip_suffixes(c, &local);
                        break;

                case IMPORT_TAR:
                        r = tar_strip_suffixes(c, &local);
                        break;

                default:
                        assert_not_reached();
                        break;
                }
                if (r < 0)
                        return log_error_errno(r, "Failed to strip suffix from URL '%s': %m", remote);

                log_info("Saving downloaded file under local name '%s'.", local);
        }

        if (blockdev && type != IMPORT_RAW)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Option 'blockdev' only available for raw images, refusing: %s", v);

        if (!GREEDY_REALLOC(arg_transfers, arg_n_transfers + 1))
                return log_oom();

        _cleanup_(sd_json_variant_unrefp) sd_json_variant *j = NULL;

        r = sd_json_buildo(
                        &j,
                        SD_JSON_BUILD_PAIR("remote", SD_JSON_BUILD_STRING(remote)),
                        SD_JSON_BUILD_PAIR("local", SD_JSON_BUILD_STRING(local)),
                        SD_JSON_BUILD_PAIR("class", JSON_BUILD_STRING_UNDERSCORIFY(image_class_to_string(class))),
                        SD_JSON_BUILD_PAIR("type", JSON_BUILD_STRING_UNDERSCORIFY(import_type_to_string(type))),
                        SD_JSON_BUILD_PAIR("readOnly", SD_JSON_BUILD_BOOLEAN(ro)),
                        SD_JSON_BUILD_PAIR("verify", JSON_BUILD_STRING_UNDERSCORIFY(import_verify_to_string(verify))));
        if (r < 0)
                return log_error_errno(r, "Failed to build import JSON object: %m");

        arg_transfers[arg_n_transfers++] = (Transfer) {
                .class = class,
                .type = type,
                .local = TAKE_PTR(local),
                .remote = TAKE_PTR(remote),
                .json = TAKE_PTR(j),
                .blockdev = blockdev,
        };

        return 0;
}

static int parse_proc_cmdline_item(const char *key, const char *value, void *data) {
        int r;

        if (proc_cmdline_key_streq(key, "systemd.pull")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = parse_pull_expression(value);
                if (r < 0)
                        log_warning_errno(r, "Failed to parse %s expression, ignoring: %s", key, value);

        } else if (proc_cmdline_key_streq(key, "systemd.pull.success_action")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                return free_and_strdup_warn(&arg_success_action, value);

        } else if (proc_cmdline_key_streq(key, "systemd.pull.failure_action")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                return free_and_strdup_warn(&arg_failure_action, value);
        }

        return 0;
}

static int parse_credentials(void) {
        _cleanup_free_ char *b = NULL;
        size_t sz = 0;
        int r;

        r = read_credential_with_decryption("import.pull", (void**) &b, &sz);
        if (r <= 0)
                return r;

        _cleanup_fclose_ FILE *f = NULL;
        f = fmemopen_unlocked(b, sz, "r");
        if (!f)
                return log_oom();

        for (;;) {
                _cleanup_free_ char *item = NULL;

                r = read_stripped_line(f, LINE_MAX, &item);
                if (r == 0)
                        break;
                if (r < 0) {
                        log_error_errno(r, "Failed to parse credential 'import.pull': %m");
                        break;
                }

                if (startswith(item, "#"))
                        continue;

                r = parse_pull_expression(item);
                if (r < 0)
                        log_warning_errno(r, "Failed to parse expression, ignoring: %s", item);
        }

        return 0;
}

static int transfer_generate(const Transfer *t, size_t c) {
        int r;

        assert(t);

        _cleanup_free_ char *service = NULL;
        if (asprintf(&service, "import%zu.service", c) < 0)
                return log_oom();

        _cleanup_fclose_ FILE *f = NULL;
        r = generator_open_unit_file(arg_dest, /* source = */ NULL, service, &f);
        if (r < 0)
                return r;

        fprintf(f,
                "[Unit]\n"
                "Description=Download of %s\n"
                "Documentation=man:systemd-import-generator(8)\n"
                "SourcePath=/proc/cmdline\n"
                "Requires=systemd-importd.socket\n"
                "After=imports-pre.target systemd-importd.socket\n"
                "Conflicts=shutdown.target\n"
                "Before=imports.target shutdown.target\n"
                "DefaultDependencies=no\n",
                t->remote);

        if (arg_success_action)
                fprintf(f, "SuccessAction=%s\n",
                        arg_success_action);

        if (arg_failure_action)
                fprintf(f, "FailureAction=%s\n",
                        arg_failure_action);

        if (t->class == IMAGE_SYSEXT)
                fputs("Before=systemd-sysext.service\n", f);
        else if (t->class == IMAGE_CONFEXT)
                fputs("Before=systemd-confext.service\n", f);

        /* Assume network resource unless URL is file:// */
        if (!file_url_is_valid(t->remote))
                fputs("Wants=network-online.target\n"
                      "After=network-online.target\n", f);

        _cleanup_free_ char *local_path = NULL, *loop_service = NULL;
        if (t->blockdev) {
                assert(t->type == IMPORT_RAW);

                local_path = strjoin(image_root_to_string(t->class), "/", t->local, ".raw");
                if (!local_path)
                        return log_oom();

                r = unit_name_from_path_instance("systemd-loop", local_path, ".service", &loop_service);
                if (r < 0)
                        return log_error_errno(r, "Failed to build systemd-loop@.service instance name from path '%s': %m", local_path);

                /* Make sure download completes before the loopback service is activated */
                fprintf(f, "Before=%s\n", loop_service);
        }

        fputs("\n"
              "[Service]\n"
              "Type=oneshot\n"
              "NotifyAccess=main\n", f);

        _cleanup_free_ char *formatted = NULL;
        r = sd_json_variant_format(t->json, /* flags= */ 0, &formatted);
        if (r < 0)
                return log_error_errno(r, "Failed to format import JSON data: %m");

        _cleanup_free_ char *escaped = specifier_escape(formatted);
        if (!escaped)
                return log_oom();

        fprintf(f, "ExecStart=:varlinkctl call -q --more /run/systemd/io.systemd.Import io.systemd.Import.Pull '%s'\n",
                escaped);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit %s: %m", service);

        r = generator_add_symlink(arg_dest, "imports.target", "wants", service);
        if (r < 0)
                return r;

        if (loop_service) {
                r = generator_add_symlink(arg_dest, "imports.target", "wants", loop_service);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int generate(void) {
        size_t c = 0;
        int r = 0;

        FOREACH_ARRAY(i, arg_transfers, arg_n_transfers)
                RET_GATHER(r, transfer_generate(i, c++));

        return r;
}

static int run(const char *dest, const char *dest_early, const char *dest_late) {
        int r;

        assert_se(arg_dest = dest);

        r = proc_cmdline_parse(parse_proc_cmdline_item, NULL, PROC_CMDLINE_RD_STRICT|PROC_CMDLINE_STRIP_RD_PREFIX);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        (void) parse_credentials();

        return generate();
}

DEFINE_MAIN_GENERATOR_FUNCTION(run);
