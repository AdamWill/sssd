/*
    Authors:
        Pavel Březina <pbrezina@redhat.com>

    Copyright (C) 2015 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>

#include "util/util.h"
#include "db/sysdb.h"
#include "tools/common/sss_tools.h"
#include "tools/common/sss_colondb.h"

#define LOCALVIEW SYSDB_LOCAL_VIEW_NAME
#define ORIGNAME "originalName"

struct override_user {
    const char *input_name;
    const char *orig_name;
    struct sss_domain_info *domain;

    const char *name;
    uid_t uid;
    gid_t gid;
    const char *home;
    const char *shell;
    const char *gecos;
};

struct override_group {
    const char *input_name;
    const char *orig_name;
    struct sss_domain_info *domain;

    const char *name;
    gid_t gid;
};

static int parse_cmdline(struct sss_cmdline *cmdline,
                         struct sss_tool_ctx *tool_ctx,
                         struct poptOption *options,
                         const char **_input_name,
                         const char **_orig_name,
                         struct sss_domain_info **_domain)
{
    enum sss_tool_opt require;
    const char *input_name;
    const char *orig_name;
    struct sss_domain_info *domain;
    int ret;

    require = options == NULL ? SSS_TOOL_OPT_OPTIONAL : SSS_TOOL_OPT_REQUIRED;

    ret = sss_tool_popt_ex(cmdline, options, require,
                           NULL, NULL, "NAME", _("Specify name of modified "
                           "object."), &input_name);
    if (ret != EXIT_SUCCESS) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse command arguments\n");
        return ret;
    }

    ret = sss_tool_parse_name(tool_ctx, tool_ctx, input_name,
                              &orig_name, &domain);
    if (ret != EOK) {
        fprintf(stderr, _("Unable to parse name %s.\n"), input_name);
        return ret;
    }

    *_input_name = input_name;
    *_orig_name = orig_name;
    *_domain = domain;

    return EXIT_SUCCESS;
}

static int parse_cmdline_user_add(struct sss_cmdline *cmdline,
                                  struct sss_tool_ctx *tool_ctx,
                                  struct override_user *user)
{
    struct poptOption options[] = {
        POPT_AUTOHELP
        {"name", 'n', POPT_ARG_STRING, &user->name, 0, _("Override name"), NULL },
        {"uid", 'u', POPT_ARG_INT, &user->uid, 0, _("Override uid (non-zero value)"), NULL },
        {"gid", 'g', POPT_ARG_INT, &user->gid, 0, _("Override gid (non-zero value)"), NULL },
        {"home", 'h', POPT_ARG_STRING, &user->home, 0, _("Override home directory"), NULL },
        {"shell", 's', POPT_ARG_STRING, &user->shell, 0, _("Override shell"), NULL },
        {"gecos", 'c', POPT_ARG_STRING, &user->gecos, 0, _("Override gecos"), NULL },
        POPT_TABLEEND
    };

    return parse_cmdline(cmdline, tool_ctx, options, &user->input_name,
                         &user->orig_name, &user->domain);
}

static int parse_cmdline_user_del(struct sss_cmdline *cmdline,
                                  struct sss_tool_ctx *tool_ctx,
                                  struct override_user *user)
{
    return parse_cmdline(cmdline, tool_ctx, NULL, &user->input_name,
                         &user->orig_name, &user->domain);
}

static int parse_cmdline_group_add(struct sss_cmdline *cmdline,
                                   struct sss_tool_ctx *tool_ctx,
                                   struct override_group *group)
{
    struct poptOption options[] = {
        POPT_AUTOHELP
        {"name", 'n', POPT_ARG_STRING, &group->name, 0, _("Override name"), NULL },
        {"gid", 'g', POPT_ARG_INT, &group->gid, 0, _("Override gid"), NULL },
        POPT_TABLEEND
    };

    return parse_cmdline(cmdline, tool_ctx, options, &group->input_name,
                         &group->orig_name, &group->domain);
}

static int parse_cmdline_group_del(struct sss_cmdline *cmdline,
                                   struct sss_tool_ctx *tool_ctx,
                                   struct override_group *group)
{
    return parse_cmdline(cmdline, tool_ctx, NULL, &group->input_name,
                         &group->orig_name, &group->domain);
}

static int parse_cmdline_import(struct sss_cmdline *cmdline,
                                struct sss_tool_ctx *tool_ctx,
                                const char **_file)
{
    int ret;

    ret = sss_tool_popt_ex(cmdline, NULL, SSS_TOOL_OPT_OPTIONAL,
                           NULL, NULL, "FILE", "File to import the data from.",
                           _file);
    if (ret != EXIT_SUCCESS) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse command arguments\n");
        return ret;
    }

    return EXIT_SUCCESS;
}

static int parse_cmdline_export(struct sss_cmdline *cmdline,
                                struct sss_tool_ctx *tool_ctx,
                                const char **_file)
{
    int ret;

    ret = sss_tool_popt_ex(cmdline, NULL, SSS_TOOL_OPT_OPTIONAL,
                           NULL, NULL, "FILE", "File to export the data to.",
                           _file);
    if (ret != EXIT_SUCCESS) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse command arguments\n");
        return ret;
    }

    return EXIT_SUCCESS;
}

static errno_t prepare_view(struct sss_domain_info *domain)
{
    char *viewname = NULL;
    errno_t ret;

    ret = sysdb_get_view_name(NULL, domain->sysdb, &viewname);
    if (ret != EOK && ret != ENOENT) {
        DEBUG(SSSDBG_OP_FAILURE, "sysdb_get_view_name() failed.\n");
        return ret;
    }

    if (ret == EOK) {
        if (is_local_view(viewname)) {
            DEBUG(SSSDBG_TRACE_FUNC, "%s view is already present.\n", viewname);
            ret = EOK;
            goto done;
        } else if (viewname != NULL) {
            DEBUG(SSSDBG_MINOR_FAILURE, "There already exists view %s. "
                  "Only one view is supported. Nothing to do.\n", viewname);
            ret = EEXIST;
            goto done;
        }
    }

    DEBUG(SSSDBG_TRACE_FUNC, "Creating %s view.\n", LOCALVIEW);

    ret = sysdb_update_view_name(domain->sysdb, LOCALVIEW);
    if (ret == EOK) {
        printf("SSSD needs to be restarted for the changes to take effect.\n");
    }

done:
    talloc_free(viewname);
    return ret;
}

errno_t prepare_view_msg(struct sss_domain_info *domain)
{
    errno_t ret;

    ret = prepare_view(domain);
    if (ret == EEXIST) {
        fprintf(stderr, _("Other than " LOCALVIEW " view already exist "
                "in domain %s.\n"), domain->name);
    } else if (ret != EOK) {
        fprintf(stderr, _("Unable to prepare " LOCALVIEW
                " view in domain %s.\n"), domain->name);
    }

    return ret;
}

static char *build_anchor(TALLOC_CTX *mem_ctx, const char *obj_dn)
{
    char *anchor;
    char *safe_dn;
    errno_t ret;

    ret = sysdb_dn_sanitize(mem_ctx, obj_dn, &safe_dn);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "sysdb_dn_sanitize() failed\n");
        return NULL;
    }

    anchor = talloc_asprintf(mem_ctx, ":%s:%s", LOCALVIEW, safe_dn);

    talloc_free(safe_dn);

    return anchor;
}

static struct sysdb_attrs *build_attrs(TALLOC_CTX *mem_ctx,
                                       const char *name,
                                       uid_t uid,
                                       gid_t gid,
                                       const char *home,
                                       const char *shell,
                                       const char *gecos)
{
    struct sysdb_attrs *attrs;
    errno_t ret;

    attrs = sysdb_new_attrs(mem_ctx);
    if (attrs == NULL) {
        return NULL;
    }

    if (name != NULL) {
        ret = sysdb_attrs_add_string(attrs, SYSDB_NAME, name);
        if (ret != EOK) {
            goto done;
        }
    }

    if (uid != 0) {
        ret = sysdb_attrs_add_uint32(attrs, SYSDB_UIDNUM, uid);
        if (ret != EOK) {
            goto done;
        }
    }

    if (gid != 0) {
        ret = sysdb_attrs_add_uint32(attrs, SYSDB_GIDNUM, gid);
        if (ret != EOK) {
            goto done;
        }
    }

    if (home != NULL) {
        ret = sysdb_attrs_add_string(attrs, SYSDB_HOMEDIR, home);
        if (ret != EOK) {
            goto done;
        }
    }

    if (shell != NULL) {
        ret = sysdb_attrs_add_string(attrs, SYSDB_SHELL, shell);
        if (ret != EOK) {
            goto done;
        }
    }

    if (gecos != NULL) {
        ret = sysdb_attrs_add_string(attrs, SYSDB_GECOS, gecos);
        if (ret != EOK) {
            goto done;
        }
    }

    ret = EOK;

done:
    if (ret != EOK) {
        talloc_free(attrs);
        return NULL;
    }

    return attrs;
}

static struct sysdb_attrs *build_user_attrs(TALLOC_CTX *mem_ctx,
                                            struct override_user *user)
{
    return build_attrs(mem_ctx, user->name, user->uid, user->gid, user->home,
                       user->shell, user->gecos);
}

static struct sysdb_attrs *build_group_attrs(TALLOC_CTX *mem_ctx,
                                             struct override_group *group)
{
    return build_attrs(mem_ctx, group->name, 0, group->gid, 0, NULL, NULL);
}

static char *get_fqname(TALLOC_CTX *mem_ctx,
                        struct sss_domain_info *domain,
                        const char *name)
{
    char *fqname;
    int fqlen;
    int check;

    if (domain == NULL) {
        return NULL;
    }

    /* Get length. */
    fqlen = sss_fqname(NULL, 0, domain->names, domain, name);
    if (fqlen > 0) {
        fqlen++; /* \0 */
    } else {
        return NULL;
    }

    fqname = talloc_zero_array(mem_ctx, char, fqlen);
    if (fqname == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "talloc_zero_array() failed\n");
        return NULL;
    }

    check = sss_fqname(fqname, fqlen, domain->names, domain, name);
    if (check < 0 || check != fqlen - 1) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Failed to generate a fully qualified name "
              "for user [%s] in [%s]! Skipping user.\n", name, domain->name);
        talloc_free(fqname);
        return NULL;
    }

    return fqname;
}

static char *get_sysname(TALLOC_CTX *mem_ctx,
                         struct sss_domain_info *domain,
                         const char *name)
{
    if (domain == NULL || !domain->fqnames) {
        return talloc_strdup(mem_ctx, name);
    }

    return get_fqname(mem_ctx, domain, name);
}

static struct sss_domain_info *
get_object_domain(enum sysdb_member_type type,
                  const char *name,
                  struct sss_domain_info *domain,
                  struct sss_domain_info *domains)
{
    TALLOC_CTX *tmp_ctx;
    struct sss_domain_info *dom;
    struct ldb_result *res;
    const char *strtype;
    char *sysname;
    bool check_next;
    errno_t ret;

    tmp_ctx = talloc_new(NULL);
    if (tmp_ctx == NULL) {
        return NULL;
    }

    sysname = get_sysname(tmp_ctx, domain, name);
    if (sysname == NULL) {
        ret = ENOMEM;
        goto done;
    }

    /* Ensure that the object is in cache. */
    switch (type) {
    case SYSDB_MEMBER_USER:
        if (getpwnam(sysname) == NULL) {
            ret = ENOENT;
            goto done;
        }
        break;
    case SYSDB_MEMBER_GROUP:
        if (getgrnam(sysname) == NULL) {
            ret = ENOENT;
            goto done;
        }
        break;
    default:
        DEBUG(SSSDBG_CRIT_FAILURE, "Unsupported member type %d\n", type);
        ret = ERR_INTERNAL;
        goto done;
    }

    /* Find domain if it is unknown. */
    if (domain == NULL) {
        check_next = true;
        dom = domains;
    } else {
        check_next = false;
        dom = domain;
    }

    do {
        switch (type) {
        case SYSDB_MEMBER_USER:
            DEBUG(SSSDBG_TRACE_FUNC, "Trying to find user %s@%s\n",
                  name, dom->name);
            ret = sysdb_getpwnam(tmp_ctx, dom, name, &res);
            strtype = "user";
            break;
        case SYSDB_MEMBER_GROUP:
            DEBUG(SSSDBG_TRACE_FUNC, "Trying to find group %s@%s\n",
                  name, dom->name);
            ret = sysdb_getgrnam(tmp_ctx, dom, name, &res);
            strtype = "group";
            break;
        default:
            DEBUG(SSSDBG_CRIT_FAILURE, "Unsupported member type %d\n", type);
            ret = ERR_INTERNAL;
            goto done;
        }

        if (ret == EOK && res->count == 0) {
            ret = ENOENT;

            if (check_next) {
                dom = dom->next;
                continue;
            }
        }

        if (ret != EOK) {
            DEBUG(SSSDBG_CRIT_FAILURE, "Unable to find %s %s@%s [%d]: %s\n",
                  strtype, name, dom->name, ret, sss_strerror(ret));
            goto done;
        } else if (res->count != 1) {
            DEBUG(SSSDBG_CRIT_FAILURE, "More than one %s found?\n", strtype);
            ret = ERR_INTERNAL;
            goto done;
        }

        check_next = false;
    } while (check_next && dom != NULL);

    if (dom == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "No domain match for %s\n", name);
        ret = ENOENT;
        goto done;
    }

    DEBUG(SSSDBG_TRACE_FUNC, "Domain of %s %s is %s\n",
          strtype, name, dom->name);

done:
    talloc_free(tmp_ctx);

    if (ret != EOK) {
        return NULL;
    }

    return dom;
}

static errno_t get_user_domain_msg(struct sss_tool_ctx *tool_ctx,
                                   struct override_user *user)
{
    struct sss_domain_info *newdom;
    const char *domname;

    newdom = get_object_domain(SYSDB_MEMBER_USER, user->orig_name,
                               user->domain, tool_ctx->domains);
    if (newdom == NULL) {
        domname = user->domain == NULL ? "[unknown]" : user->domain->name;
        fprintf(stderr, _("Unable to find user %s@%s.\n"),
                user->orig_name, domname);
        return ENOENT;
    }

    user->domain = newdom;
    return EOK;
}

static errno_t get_group_domain_msg(struct sss_tool_ctx *tool_ctx,
                                    struct override_group *group)
{
    struct sss_domain_info *newdom;
    const char *domname;

    newdom = get_object_domain(SYSDB_MEMBER_GROUP, group->orig_name,
                               group->domain, tool_ctx->domains);
    if (newdom == NULL) {
        domname = group->domain == NULL ? "[unknown]" : group->domain->name;
        fprintf(stderr, _("Unable to find group %s@%s.\n"),
                group->orig_name, domname);
        return ENOENT;
    }

    group->domain = newdom;
    return EOK;
}

static errno_t get_object_dn(TALLOC_CTX *mem_ctx,
                             struct sss_domain_info *domain,
                             enum sysdb_member_type type,
                             const char *name,
                             struct ldb_dn **_ldb_dn,
                             const char **_str_dn)
{
    struct ldb_dn *ldb_dn;

    switch (type) {
    case SYSDB_MEMBER_USER:
       ldb_dn = sysdb_user_dn(mem_ctx, domain, name);
       break;
    case SYSDB_MEMBER_GROUP:
       ldb_dn = sysdb_group_dn(mem_ctx, domain, name);
       break;
    default:
       DEBUG(SSSDBG_CRIT_FAILURE, "Unsupported member type %d\n", type);
       return ERR_INTERNAL;
    }

    if (ldb_dn == NULL) {
        return ENOMEM;
    }

    if (_str_dn != NULL) {
        *_str_dn = ldb_dn_get_linearized(ldb_dn);
    }

    if (_ldb_dn != NULL) {
        *_ldb_dn = ldb_dn;
    } else {
        talloc_free(ldb_dn);
    }

    return EOK;
}

static errno_t override_object_add(struct sss_domain_info *domain,
                                   enum sysdb_member_type type,
                                   struct sysdb_attrs *attrs,
                                   const char *name)
{
    TALLOC_CTX *tmp_ctx;
    const char *anchor;
    struct ldb_dn *ldb_dn;
    const char *str_dn;
    errno_t ret;

    tmp_ctx = talloc_new(NULL);
    if (tmp_ctx == NULL) {
        return ENOMEM;
    }

    ret = get_object_dn(tmp_ctx, domain, type, name, &ldb_dn, &str_dn);
    if (ret != EOK) {
        goto done;
    }

    anchor = build_anchor(tmp_ctx, str_dn);
    if (anchor == NULL) {
        ret = ENOMEM;
        goto done;
    }

    ret = sysdb_attrs_add_string(attrs, SYSDB_OVERRIDE_ANCHOR_UUID, anchor);
    if (ret != EOK) {
        goto done;
    }

    DEBUG(SSSDBG_TRACE_FUNC, "Creating override for %s\n", str_dn);

    ret = sysdb_store_override(domain, LOCALVIEW, type, attrs, ldb_dn);

done:
    talloc_free(tmp_ctx);
    return ret;
}

static errno_t override_fqn(TALLOC_CTX *mem_ctx,
                            struct sss_tool_ctx *tool_ctx,
                            struct sss_domain_info *domain,
                            const char *input,
                            const char **_name)
{
    struct sss_domain_info *dom;
    errno_t ret;

    if (input == NULL) {
        return EOK;
    }

    ret = sss_tool_parse_name(mem_ctx, tool_ctx, input, _name, &dom);
    if (ret == EAGAIN) {
        DEBUG(SSSDBG_OP_FAILURE, "Unable to find domain from "
              "fqn %s\n", input);
        fprintf(stderr, _("Changing domain is not allowed!\n"));
        ret = EINVAL;
    } else if (ret == EOK && dom != NULL && dom != domain) {
        DEBUG(SSSDBG_OP_FAILURE, "Trying to change domain from "
              "%s to %s, not allowed!\n", domain->name, dom->name);
        fprintf(stderr, _("Changing domain is not allowed!\n"));
        ret = EINVAL;
    } else if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse name %s [%d]: %s\n",
              input, ret, sss_strerror(ret));
    }

    return ret;
}

static errno_t override_user(struct sss_tool_ctx *tool_ctx,
                             struct override_user *input_user)
{
    TALLOC_CTX *tmp_ctx;
    struct override_user user;
    struct sysdb_attrs *attrs;
    errno_t ret;

    tmp_ctx = talloc_new(NULL);
    if (tmp_ctx == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "talloc_new() failed\n");
        return ENOMEM;
    }

    user = *input_user;

    /* We need to parse the name and ensure that domain did not change. */
    ret = override_fqn(tmp_ctx, tool_ctx, user.domain, user.name, &user.name);
    if (ret != EOK) {
        goto done;
    }

    ret = prepare_view_msg(user.domain);
    if (ret != EOK) {
        goto done;
    }

    attrs = build_user_attrs(tool_ctx, &user);
    if (attrs == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to build sysdb attrs.\n");
        ret = ENOMEM;
        goto done;
    }

    ret = override_object_add(user.domain, SYSDB_MEMBER_USER, attrs,
                              user.orig_name);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to add override object.\n");
        goto done;
    }

    ret = EOK;

done:
    talloc_free(tmp_ctx);
    return ret;
}

static errno_t override_group(struct sss_tool_ctx *tool_ctx,
                              struct override_group *input_group)
{
    TALLOC_CTX *tmp_ctx;
    struct override_group group;
    struct sysdb_attrs *attrs;
    errno_t ret;

    tmp_ctx = talloc_new(NULL);
    if (tmp_ctx == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "talloc_new() failed\n");
        return ENOMEM;
    }

    group = *input_group;

    /* We need to parse the name and ensure that domain did not change. */
    ret = override_fqn(tmp_ctx, tool_ctx, group.domain, group.name,
                       &group.name);
    if (ret != EOK) {
        goto done;
    }

    ret = prepare_view_msg(group.domain);
    if (ret != EOK) {
        goto done;
    }

    attrs = build_group_attrs(tool_ctx, &group);
    if (attrs == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to build sysdb attrs.\n");
        ret = ENOMEM;
        goto done;
    }

    ret = override_object_add(group.domain, SYSDB_MEMBER_GROUP, attrs,
                              group.orig_name);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to add override object.\n");
        goto done;
    }

    ret = EOK;

done:
    talloc_free(tmp_ctx);
    return ret;
}

static errno_t override_object_del(struct sss_domain_info *domain,
                                   enum sysdb_member_type type,
                                   const char *name)
{
    TALLOC_CTX *tmp_ctx;
    struct ldb_message *msg;
    struct ldb_dn *override_dn;
    struct ldb_dn *ldb_dn;
    const char *str_dn;
    const char *anchor;
    errno_t ret;
    int sret;
    bool in_transaction = false;
    struct ldb_context *ldb = sysdb_ctx_get_ldb(domain->sysdb);

    tmp_ctx = talloc_new(NULL);
    if (tmp_ctx == NULL) {
        return ENOMEM;
    }

    ret = get_object_dn(tmp_ctx, domain, type, name, &ldb_dn, &str_dn);
    if (ret != EOK) {
        goto done;
    }

    anchor = build_anchor(tmp_ctx, str_dn);
    if (anchor == NULL) {
        ret = ENOMEM;
        goto done;
    }

    override_dn = ldb_dn_new_fmt(tmp_ctx, ldb,
                        SYSDB_TMPL_OVERRIDE, anchor, LOCALVIEW);
    if (override_dn == NULL) {
        ret = ENOMEM;
        goto done;
    }

    DEBUG(SSSDBG_TRACE_FUNC, "Removing override for %s\n", str_dn);

    ret = sysdb_transaction_start(domain->sysdb);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, "sysdb_transaction_start() failed.\n");
        goto done;
    }
    in_transaction = true;

    ret = sysdb_delete_entry(domain->sysdb, override_dn, true);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, "sysdb_delete_entry() failed.\n");
        goto done;
    }

    msg = ldb_msg_new(tmp_ctx);
    if (msg == NULL) {
        ret = ENOMEM;
        goto done;
    }

    msg->dn = talloc_steal(msg, ldb_dn);
    if (msg->dn == NULL) {
        ret = ENOMEM;
        goto done;
    }

    ret = ldb_msg_add_empty(msg, SYSDB_OVERRIDE_DN, LDB_FLAG_MOD_DELETE, NULL);
    if (ret != LDB_SUCCESS) {
        DEBUG(SSSDBG_OP_FAILURE, "ldb_msg_add_empty() failed\n");
        ret = sysdb_error_to_errno(ret);
        goto done;
    }

    ret = ldb_modify(ldb, msg);
    if (ret != LDB_SUCCESS && ret != LDB_ERR_NO_SUCH_ATTRIBUTE) {
        DEBUG(SSSDBG_OP_FAILURE,
              "ldb_modify() failed: [%s](%d)[%s]\n",
              ldb_strerror(ret), ret, ldb_errstring(ldb));
        ret = sysdb_error_to_errno(ret);
        goto done;
    }

    ret = sysdb_transaction_commit(domain->sysdb);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Failed to commit transaction\n");
        goto done;
    }
    in_transaction = false;

    ret = EOK;

done:
    if (in_transaction) {
        sret = sysdb_transaction_cancel(domain->sysdb);
        if (sret != EOK) {
            DEBUG(SSSDBG_CRIT_FAILURE, "Could not cancel transaction\n");
        }
    }

    talloc_free(tmp_ctx);
    return ret;
}

static errno_t append_name(struct sss_domain_info *domain,
                           struct ldb_message *override)
{
    TALLOC_CTX *tmp_ctx;
    struct ldb_context *ldb = sysdb_ctx_get_ldb(domain->sysdb);
    struct ldb_dn *dn;
    struct ldb_message **msgs;
    const char *attrs[] = {SYSDB_NAME, NULL};
    const char *name;
    const char *fqname;
    size_t count;
    errno_t ret;

    tmp_ctx = talloc_new(NULL);
    if (tmp_ctx == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "talloc_new() failed.\n");
        return ENOMEM;
    }

    dn = ldb_msg_find_attr_as_dn(ldb, tmp_ctx, override,
                                 SYSDB_OVERRIDE_OBJECT_DN);
    if (dn == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Missing overrideObjectDN?\n");
        ret = ERR_INTERNAL;
        goto done;
    }

    ret = sysdb_search_entry(tmp_ctx, domain->sysdb, dn, LDB_SCOPE_BASE,
                             NULL, attrs, &count, &msgs);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "sysdb_search_entry() failed [%d]: %s\n",
              ret, sss_strerror(ret));
        goto done;
    } else if (count != 1) {
        DEBUG(SSSDBG_CRIT_FAILURE, "More than one user found?\n");
        ret = ERR_INTERNAL;
        goto done;
    }

    name = ldb_msg_find_attr_as_string(msgs[0], SYSDB_NAME, NULL);
    if (name == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Object with no name?\n");
        ret = ERR_INTERNAL;
        goto done;
    }

    fqname = get_fqname(tmp_ctx, domain, name);
    if (fqname == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to get fqname\n");
        ret = ENOMEM;
        goto done;
    }

    ret = ldb_msg_add_string(override, ORIGNAME, fqname);
    if (ret != LDB_SUCCESS) {
        ret = sysdb_error_to_errno(ret);
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to add attribute to msg\n");
        goto done;
    }

    talloc_steal(override, fqname);

done:
    talloc_free(tmp_ctx);

    return ret;
}

static errno_t list_overrides(TALLOC_CTX *mem_ctx,
                              const char *filter,
                              const char **attrs,
                              struct sss_domain_info *domain,
                              size_t *_count,
                              struct ldb_message ***_msgs)
{
    TALLOC_CTX *tmp_ctx;
    struct ldb_dn *dn;
    struct ldb_context *ldb = sysdb_ctx_get_ldb(domain->sysdb);
    size_t count;
    struct ldb_message **msgs;
    size_t i;
    int ret;

    tmp_ctx = talloc_new(NULL);
    if (tmp_ctx == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "talloc_new() failed.\n");
        return ENOMEM;
    }

    /* Acquire list of override objects. */
    dn = ldb_dn_new_fmt(tmp_ctx, ldb, SYSDB_TMPL_VIEW_SEARCH_BASE, LOCALVIEW);
    if (dn == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, "ldb_dn_new_fmt() failed.\n");
        ret = EIO;
        goto done;
    }

    ret = sysdb_search_entry(tmp_ctx, domain->sysdb, dn, LDB_SCOPE_SUBTREE,
                             filter, attrs, &count, &msgs);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "sysdb_search_entry() failed [%d]: %s\n",
              ret, sss_strerror(ret));
        goto done;
    }

    /* Amend messages with original name. */
    for (i = 0; i < count; i++) {
        ret = append_name(domain, msgs[i]);
        if (ret != EOK) {
            DEBUG(SSSDBG_CRIT_FAILURE, "Unable to append name [%d]: %s\n",
                  ret, sss_strerror(ret));
            goto done;
        }
    }

    *_msgs = talloc_steal(mem_ctx, msgs);
    *_count = count;

    ret = EOK;

done:
    talloc_free(tmp_ctx);

    return ret;
}

static struct override_user *
list_user_overrides(TALLOC_CTX *mem_ctx,
                    struct sss_domain_info *domain)
{
    TALLOC_CTX *tmp_ctx;
    struct override_user *objs;
    struct ldb_message **msgs;
    size_t count;
    size_t i;
    errno_t ret;
    const char *attrs[] = SYSDB_PW_ATTRS;

    tmp_ctx = talloc_new(NULL);
    if (tmp_ctx == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "talloc_new() failed.\n");
        return NULL;
    }

    ret = list_overrides(tmp_ctx, "(objectClass=" SYSDB_OVERRIDE_USER_CLASS ")",
                         attrs, domain, &count, &msgs);
    if (ret != EOK) {
        goto done;
    }

    objs = talloc_zero_array(tmp_ctx, struct override_user, count + 1);
    if (objs == NULL) {
        ret = ENOMEM;
        goto done;
    }

    for (i = 0; i < count; i++) {
        objs[i].orig_name = ldb_msg_find_attr_as_string(msgs[i], ORIGNAME,
                                                        NULL);
        if (objs[i].orig_name == NULL) {
            DEBUG(SSSDBG_CRIT_FAILURE, "Missing name?!\n");
            ret = ERR_INTERNAL;
            goto done;
        }

        objs[i].name = ldb_msg_find_attr_as_string(msgs[i], SYSDB_NAME, NULL);
        objs[i].uid = ldb_msg_find_attr_as_uint(msgs[i], SYSDB_UIDNUM, 0);
        objs[i].gid = ldb_msg_find_attr_as_uint(msgs[i], SYSDB_GIDNUM, 0);
        objs[i].home = ldb_msg_find_attr_as_string(msgs[i], SYSDB_HOMEDIR, NULL);
        objs[i].shell = ldb_msg_find_attr_as_string(msgs[i], SYSDB_SHELL, NULL);
        objs[i].gecos = ldb_msg_find_attr_as_string(msgs[i], SYSDB_GECOS, NULL);
    }

    talloc_steal(mem_ctx, objs);

done:
    talloc_free(tmp_ctx);

    if (ret != EOK) {
        return NULL;
    }

    return objs;
}

static struct override_group *
list_group_overrides(TALLOC_CTX *mem_ctx,
                     struct sss_domain_info *domain)
{
    TALLOC_CTX *tmp_ctx;
    struct override_group *objs;
    struct ldb_message **msgs;
    size_t count;
    size_t i;
    errno_t ret;
    const char *attrs[] = SYSDB_GRSRC_ATTRS;

    tmp_ctx = talloc_new(NULL);
    if (tmp_ctx == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "talloc_new() failed.\n");
        return NULL;
    }

    ret = list_overrides(tmp_ctx, "(objectClass=" SYSDB_OVERRIDE_GROUP_CLASS ")",
                         attrs, domain, &count, &msgs);
    if (ret != EOK) {
        goto done;
    }

    objs = talloc_zero_array(tmp_ctx, struct override_group, count + 1);
    if (objs == NULL) {
        ret = ENOMEM;
        goto done;
    }

    for (i = 0; i < count; i++) {
        objs[i].orig_name = ldb_msg_find_attr_as_string(msgs[i], ORIGNAME,
                                                        NULL);
        if (objs[i].orig_name == NULL) {
            DEBUG(SSSDBG_CRIT_FAILURE, "Missing name?!\n");
            ret = ERR_INTERNAL;
            goto done;
        }

        objs[i].name = ldb_msg_find_attr_as_string(msgs[i], SYSDB_NAME, NULL);
        objs[i].gid = ldb_msg_find_attr_as_uint(msgs[i], SYSDB_GIDNUM, 0);
    }

    talloc_steal(mem_ctx, objs);

done:
    talloc_free(tmp_ctx);

    if (ret != EOK) {
        return NULL;
    }

    return objs;
}

static int override_user_add(struct sss_cmdline *cmdline,
                             struct sss_tool_ctx *tool_ctx,
                             void *pvt)
{
    struct override_user user = {NULL};
    int ret;

    ret = parse_cmdline_user_add(cmdline, tool_ctx, &user);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse command line.\n");
        return EXIT_FAILURE;
    }

    ret = get_user_domain_msg(tool_ctx, &user);
    if (ret != EOK) {
        return EXIT_FAILURE;
    }

    ret = override_user(tool_ctx, &user);
    if (ret != EOK) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int override_user_del(struct sss_cmdline *cmdline,
                             struct sss_tool_ctx *tool_ctx,
                             void *pvt)
{
    struct override_user user = {NULL};
    int ret;

    ret = parse_cmdline_user_del(cmdline, tool_ctx, &user);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse command line.\n");
        return EXIT_FAILURE;
    }

    ret = get_user_domain_msg(tool_ctx, &user);
    if (ret != EOK) {
        return EXIT_FAILURE;
    }

    ret = override_object_del(user.domain, SYSDB_MEMBER_USER, user.orig_name);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to delete override object.\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int override_user_import(struct sss_cmdline *cmdline,
                                struct sss_tool_ctx *tool_ctx,
                                void *pvt)
{
    TALLOC_CTX *tmp_ctx;
    struct sss_colondb *db;
    const char *filename;
    struct override_user obj;
    int linenum = 1;
    errno_t ret;
    int exit;

    tmp_ctx = talloc_new(NULL);
    if (tmp_ctx == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "talloc_new() failed.\n");
        return EXIT_FAILURE;
    }

    /**
     * Format: orig_name:name:uid:gid:gecos:home:shell
     */
    struct sss_colondb_read_field table[] = {
        {SSS_COLONDB_STRING, {.str = &obj.input_name}},
        {SSS_COLONDB_STRING, {.str = &obj.name}},
        {SSS_COLONDB_UINT32, {.uint32 = &obj.uid}},
        {SSS_COLONDB_UINT32, {.uint32 = &obj.gid}},
        {SSS_COLONDB_STRING, {.str = &obj.gecos}},
        {SSS_COLONDB_STRING, {.str = &obj.home}},
        {SSS_COLONDB_STRING, {.str = &obj.shell}},
        {SSS_COLONDB_SENTINEL, {0}}
    };

    ret = parse_cmdline_import(cmdline, tool_ctx, &filename);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse command line.\n");
        exit = EXIT_FAILURE;
        goto done;
    }

    db = sss_colondb_open(tool_ctx, SSS_COLONDB_READ, filename);
    if (db == NULL) {
        fprintf(stderr, _("Unable to open %s.\n"), filename);
        exit = EXIT_FAILURE;
        goto done;
    }

    while ((ret = sss_colondb_readline(tmp_ctx, db, table)) == EOK) {
        linenum++;

        ret = sss_tool_parse_name(tool_ctx, tool_ctx, obj.input_name,
                                  &obj.orig_name, &obj.domain);
        if (ret != EOK) {
            fprintf(stderr, _("Unable to parse name %s.\n"), obj.input_name);
            exit = EXIT_FAILURE;
            goto done;
        }

        ret = get_user_domain_msg(tool_ctx, &obj);
        if (ret != EOK) {
            exit = EXIT_FAILURE;
            goto done;
        }

        ret = override_user(tool_ctx, &obj);
        if (ret != EOK) {
            exit = EXIT_FAILURE;
            goto done;
        }

        talloc_free_children(tmp_ctx);
    }

    if (ret != EOF) {
        fprintf(stderr, _("Invalid format on line %d. "
                "Use --debug option for more information.\n"), linenum);
        exit = EXIT_FAILURE;
        goto done;
    }

    exit = EXIT_SUCCESS;

done:
    talloc_free(tmp_ctx);
    return exit;
}

static int override_user_export(struct sss_cmdline *cmdline,
                                struct sss_tool_ctx *tool_ctx,
                                void *pvt)
{
    struct sss_colondb *db;
    const char *filename;
    struct override_user *objs;
    struct sss_domain_info *dom;
    errno_t ret;
    int exit;
    int i;

    ret = parse_cmdline_export(cmdline, tool_ctx, &filename);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse command line.\n");
        exit = EXIT_FAILURE;
        goto done;
    }

    db = sss_colondb_open(tool_ctx, SSS_COLONDB_WRITE, filename);
    if (db == NULL) {
        fprintf(stderr, _("Unable to open %s.\n"), filename);
        exit = EXIT_FAILURE;
        goto done;
    }

    dom = tool_ctx->domains;
    do {
        objs = list_user_overrides(tool_ctx, tool_ctx->domains);
        if (objs == NULL) {
            DEBUG(SSSDBG_CRIT_FAILURE, "Unable to get override objects\n");
            exit = EXIT_FAILURE;
            goto done;
        }

        for (i = 0; objs[i].orig_name != NULL; i++) {
            /**
             * Format: orig_name:name:uid:gid:gecos:home:shell
             */
            struct sss_colondb_write_field table[] = {
                {SSS_COLONDB_STRING, {.str = objs[i].orig_name}},
                {SSS_COLONDB_STRING, {.str = objs[i].name}},
                {SSS_COLONDB_UINT32, {.uint32 = objs[i].uid}},
                {SSS_COLONDB_UINT32, {.uint32 = objs[i].gid}},
                {SSS_COLONDB_STRING, {.str = objs[i].gecos}},
                {SSS_COLONDB_STRING, {.str = objs[i].home}},
                {SSS_COLONDB_STRING, {.str = objs[i].shell}},
                {SSS_COLONDB_SENTINEL, {0}}
            };

            ret = sss_colondb_writeline(db, table);
            if (ret != EOK) {
                DEBUG(SSSDBG_CRIT_FAILURE, "Unable to write line to db\n");
                exit = EXIT_FAILURE;
                goto done;
            }
        }

        /* All overrides are under the same subtree, so we don't want to
         * descent into subdomains. */
        dom = get_next_domain(dom, false);
    } while (dom != NULL);

    exit = EXIT_SUCCESS;

done:
    return exit;
}

static int override_group_add(struct sss_cmdline *cmdline,
                              struct sss_tool_ctx *tool_ctx,
                              void *pvt)
{
    struct override_group group = {NULL};
    int ret;

    ret = parse_cmdline_group_add(cmdline, tool_ctx, &group);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse command line.\n");
        return EXIT_FAILURE;
    }

    ret = get_group_domain_msg(tool_ctx, &group);
    if (ret != EOK) {
        return EXIT_FAILURE;
    }

    ret = override_group(tool_ctx, &group);
    if (ret != EOK) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int override_group_del(struct sss_cmdline *cmdline,
                              struct sss_tool_ctx *tool_ctx,
                              void *pvt)
{
    struct override_group group = {NULL};
    int ret;

    ret = parse_cmdline_group_del(cmdline, tool_ctx, &group);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse command line.\n");
        return EXIT_FAILURE;
    }

    ret = get_group_domain_msg(tool_ctx, &group);
    if (ret != EOK) {
        return EXIT_FAILURE;
    }

    ret = override_object_del(group.domain, SYSDB_MEMBER_GROUP,
                              group.orig_name);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to delete override object.\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int override_group_import(struct sss_cmdline *cmdline,
                                 struct sss_tool_ctx *tool_ctx,
                                 void *pvt)
{
    TALLOC_CTX *tmp_ctx;
    struct sss_colondb *db;
    const char *filename;
    struct override_group obj;
    int linenum = 1;
    errno_t ret;
    int exit;

    tmp_ctx = talloc_new(NULL);
    if (tmp_ctx == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "talloc_new() failed.\n");
        return EXIT_FAILURE;
    }

    /**
     * Format: orig_name:name:gid
     */
    struct sss_colondb_read_field table[] = {
        {SSS_COLONDB_STRING, {.str = &obj.input_name}},
        {SSS_COLONDB_STRING, {.str = &obj.name}},
        {SSS_COLONDB_UINT32, {.uint32 = &obj.gid}},
        {SSS_COLONDB_SENTINEL, {0}}
    };

    ret = parse_cmdline_import(cmdline, tool_ctx, &filename);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse command line.\n");
        exit = EXIT_FAILURE;
        goto done;
    }

    db = sss_colondb_open(tool_ctx, SSS_COLONDB_READ, filename);
    if (db == NULL) {
        fprintf(stderr, _("Unable to open %s.\n"), filename);
        exit = EXIT_FAILURE;
        goto done;
    }

    while ((ret = sss_colondb_readline(tmp_ctx, db, table)) == EOK) {
        linenum++;

        ret = sss_tool_parse_name(tool_ctx, tool_ctx, obj.input_name,
                                  &obj.orig_name, &obj.domain);
        if (ret != EOK) {
            fprintf(stderr, _("Unable to parse name %s.\n"), obj.input_name);
            exit = EXIT_FAILURE;
            goto done;
        }

        ret = get_group_domain_msg(tool_ctx, &obj);
        if (ret != EOK) {
            exit = EXIT_FAILURE;
            goto done;
        }

        ret = override_group(tool_ctx, &obj);
        if (ret != EOK) {
            exit = EXIT_FAILURE;
            goto done;
        }

        talloc_free_children(tmp_ctx);
    }

    if (ret != EOF) {
        fprintf(stderr, _("Invalid format on line %d. "
                "Use --debug option for more information.\n"), linenum);
        exit = EXIT_FAILURE;
        goto done;
    }

    exit = EXIT_SUCCESS;

done:
    talloc_free(tmp_ctx);
    return exit;
}

static int override_group_export(struct sss_cmdline *cmdline,
                                 struct sss_tool_ctx *tool_ctx,
                                 void *pvt)
{
    struct sss_colondb *db;
    const char *filename;
    struct override_group *objs;
    struct sss_domain_info *dom;
    errno_t ret;
    int exit;
    int i;

    ret = parse_cmdline_export(cmdline, tool_ctx, &filename);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse command line.\n");
        exit = EXIT_FAILURE;
        goto done;
    }

    db = sss_colondb_open(tool_ctx, SSS_COLONDB_WRITE, filename);
    if (db == NULL) {
        fprintf(stderr, _("Unable to open %s.\n"), filename);
        exit = EXIT_FAILURE;
        goto done;
    }

    dom = tool_ctx->domains;
    do {
        objs = list_group_overrides(tool_ctx, tool_ctx->domains);
        if (objs == NULL) {
            DEBUG(SSSDBG_CRIT_FAILURE, "Unable to get override objects\n");
            exit = EXIT_FAILURE;
            goto done;
        }

        for (i = 0; objs[i].orig_name != NULL; i++) {
            /**
             * Format: orig_name:name:uid:gid:gecos:home:shell
             */
            struct sss_colondb_write_field table[] = {
                {SSS_COLONDB_STRING, {.str = objs[i].orig_name}},
                {SSS_COLONDB_STRING, {.str = objs[i].name}},
                {SSS_COLONDB_UINT32, {.uint32 = objs[i].gid}},
                {SSS_COLONDB_SENTINEL, {0}}
            };

            ret = sss_colondb_writeline(db, table);
            if (ret != EOK) {
                DEBUG(SSSDBG_CRIT_FAILURE, "Unable to write line to db\n");
                exit = EXIT_FAILURE;
                goto done;
            }
        }

        /* All overrides are under the same subtree, so we don't want to
         * descent into subdomains. */
        dom = get_next_domain(dom, false);
    } while (dom != NULL);

    exit = EXIT_SUCCESS;

done:
    return exit;
}

int main(int argc, const char **argv)
{
    struct sss_route_cmd commands[] = {
        {"user-add", override_user_add},
        {"user-del", override_user_del},
        {"user-import", override_user_import},
        {"user-export", override_user_export},
        {"group-add", override_group_add},
        {"group-del", override_group_del},
        {"group-import", override_group_import},
        {"group-export", override_group_export},
        {NULL, NULL}
    };

    return sss_tool_main(argc, argv, commands, NULL);
}