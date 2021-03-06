/****
 * Copyright 2006-2012 by Nils Maier
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Contributors:
 *   Nils Maier <testnutzer123@gmail.com>
 *   Ben Timby - URL decoding
 *   Jake Rhee - X-SENDFILE-TEMPORARY
 ****/

/****
 * mod_xsendfile.c: Process X-SENDFILE header cgi/scripts may set
 *     Written by Nils Maier, March 2006
 *
 * Whenever an X-SENDFILE header occures in the response headers drop
 * the body and send the replacement file idenfitied by this header instead.
 *
 * Method inspired by lighttpd <http://lighttpd.net/>
 * Code inspired by mod_headers, mod_rewrite and such
 *
 * Installation:
 *     apxs2 -cia mod_xsendfile.c
 ****/

/* Version: 1.0 */

#include "apr.h"
#include "apr_lib.h"
#include "apr_strings.h"
#include "apr_buckets.h"
#include "apr_file_io.h"

#include "apr_hash.h"
#define APR_WANT_IOVEC
#define APR_WANT_STRFUNC
#include "apr_want.h"

#include "httpd.h"
#include "http_log.h"
#include "http_config.h"
#include "http_log.h"
#define CORE_PRIVATE
#include "http_request.h"
#include "http_core.h" /* needed for per-directory core-config */
#include "util_filter.h"
#include "http_protocol.h" /* ap_hook_insert_error_filter */

#define HACKY_GZIP 1

#ifdef MOD_XSENDFILE_AUTO_GZIP
#include "zlib.h"


/* The only real dependency on ZUTIL.H is for the OS_CODE define */
/* ( which is part of the LZ77 deflate() header ) but the OS_CODE */
/* definitions are complex so for now, ZUTIL.H has to be required. */

#include "zutil.h" /* Contains OS_CODE definition(s) */

#define MOD_XSENDFILE_ZLIB_WINDOWSIZE -15
#define MOD_XSENDFILE_ZLIB_CFACTOR    9
#define MOD_XSENDFILE_ZLIB_BSIZE      8096

/* ZLIB's deflate() compression algorithm uses the same */
/* 0-9 based scale that GZIP does where '1' is 'Best speed' */
/* and '9' is 'Best compression'. since the compressed files */
/* are essentially cached, using a default value of 9 */

#define MOD_XSENDFILE_DEFLATE_DEFAULT_COMPRESSION_LEVEL 9

static int zlib_gzip_magic[2] = { 0x1f, 0x8b };

typedef struct zlib_context_t
{
    z_stream strm;
    char buffer[MOD_XSENDFILE_ZLIB_BSIZE];
    unsigned long crc;
} zlib_context_t;

#endif

#ifdef HACKY_GZIP
#define MOD_XSENDFILE_AUTO_GZIP 1
#include <unistd.h>
#endif

#define AP_XSENDFILE_HEADER "X-SENDFILE"
#define AP_XSENDFILETEMPORARY_HEADER "X-SENDFILE-TEMPORARY"

module AP_MODULE_DECLARE_DATA xsendfile_module;

typedef enum {
  XSENDFILE_UNSET = 0,
  XSENDFILE_ENABLED = 1<<0,
  XSENDFILE_DISABLED = 1<<1
} xsendfile_conf_active_t;

typedef struct xsendfile_conf_t {
  xsendfile_conf_active_t enabled;
  xsendfile_conf_active_t ignoreETag;
  xsendfile_conf_active_t ignoreLM;
  xsendfile_conf_active_t unescape;
  apr_array_header_t *paths;
  apr_array_header_t *temporaryPaths;
} xsendfile_conf_t;

/* structure to hold the path and permissions */
typedef struct xsendfile_path_t {
  const char *path;
  int allowFileDelete;
} xsendfile_path_t;

static xsendfile_conf_t *xsendfile_config_create(apr_pool_t *p) {
  xsendfile_conf_t *conf;

  conf = (xsendfile_conf_t *) apr_pcalloc(p, sizeof(xsendfile_conf_t));
  conf->unescape =
    conf->ignoreETag =
    conf->ignoreLM =
    conf->enabled =
    XSENDFILE_UNSET;

  conf->paths = apr_array_make(p, 1, sizeof(xsendfile_path_t));

  return conf;
}

static void *xsendfile_config_server_create(apr_pool_t *p, server_rec *s) {
  return (void*)xsendfile_config_create(p);
}

#define XSENDFILE_CFLAG(x) conf->x = overrides->x != XSENDFILE_UNSET ? overrides->x : base->x

static void *xsendfile_config_merge(apr_pool_t *p, void *basev, void *overridesv) {
  xsendfile_conf_t *base = (xsendfile_conf_t *)basev;
  xsendfile_conf_t *overrides = (xsendfile_conf_t *)overridesv;
  xsendfile_conf_t *conf;

  conf = (xsendfile_conf_t *) apr_pcalloc(p, sizeof(xsendfile_conf_t));

  XSENDFILE_CFLAG(enabled);
  XSENDFILE_CFLAG(ignoreETag);
  XSENDFILE_CFLAG(ignoreLM);
  XSENDFILE_CFLAG(unescape);

  conf->paths = apr_array_append(p, overrides->paths, base->paths);

  return (void*)conf;
}
#undef XSENDFILE_CFLAG

static void *xsendfile_config_perdir_create(apr_pool_t *p, char *path) {
  return (void*)xsendfile_config_create(p);
}

static const char *xsendfile_cmd_flag(cmd_parms *cmd, void *perdir_confv,
    int flag) {
  xsendfile_conf_t *conf = (xsendfile_conf_t *)perdir_confv;
  if (!cmd->path) {
    conf = (xsendfile_conf_t*)ap_get_module_config(
      cmd->server->module_config,
      &xsendfile_module
      );
  }
  if (!conf) {
    return "Cannot get configuration object";
  }
  if (!strcasecmp(cmd->cmd->name, "xsendfile")) {
    conf->enabled = flag ? XSENDFILE_ENABLED : XSENDFILE_DISABLED;
  }
  else if (!strcasecmp(cmd->cmd->name, "xsendfileignoreetag")) {
    conf->ignoreETag = flag ? XSENDFILE_ENABLED: XSENDFILE_DISABLED;
  }
  else if (!strcasecmp(cmd->cmd->name, "xsendfileignorelastmodified")) {
    conf->ignoreLM = flag ? XSENDFILE_ENABLED: XSENDFILE_DISABLED;
  }
  else if (!strcasecmp(cmd->cmd->name, "xsendfileunescape")) {
    conf->unescape = flag ? XSENDFILE_ENABLED: XSENDFILE_DISABLED;
  }
  else {
    return apr_psprintf(
      cmd->pool,
      "Not a valid command in this context: %s %s",
      cmd->cmd->name,
      flag ? "On": "Off"
      );
  }

  return NULL;
}

static const char *xsendfile_cmd_path(cmd_parms *cmd, void *pdc,
    const char *path, const char *allowFileDelete) {
  xsendfile_conf_t *conf = (xsendfile_conf_t*)ap_get_module_config(
    cmd->server->module_config,
    &xsendfile_module
    );

  xsendfile_path_t *newpath = (xsendfile_path_t*)apr_array_push(conf->paths);
  newpath->path = apr_pstrdup(cmd->pool, path);
  newpath->allowFileDelete = (allowFileDelete && strcmp(allowFileDelete, "AllowFileDelete") == 0) ? 1: 0;

  return NULL;
}

/*
  little helper function to get the original request path
  code borrowed from request.c and util_script.c
*/
static const char *ap_xsendfile_get_orginal_path(request_rec *rec) {
  const char
    *rv = rec->the_request,
    *last;

  int dir = 0;
  size_t uri_len;

  /* skip method && spaces */
  while (*rv && !apr_isspace(*rv)) {
    ++rv;
  }
  while (apr_isspace(*rv)) {
    ++rv;
  }
  /* first space is the request end */
  last = rv;
  while (*last && !apr_isspace(*last)) {
    ++last;
  }
  uri_len = last - rv;
  if (!uri_len) {
    return NULL;
  }

  /* alright, lets see if the request_uri changed! */
  if (strncmp(rv, rec->uri, uri_len) == 0) {
    rv = apr_pstrdup(rec->pool, rec->filename);
    dir = rec->finfo.filetype == APR_DIR;
  }
  else {
    /* need to lookup the url again as it changed */
    request_rec *sr = ap_sub_req_lookup_uri(
      apr_pstrmemdup(rec->pool, rv, uri_len),
      rec,
      NULL
      );
    if (!sr) {
      return NULL;
    }
    rv = apr_pstrdup(rec->pool, sr->filename);
    dir = rec->finfo.filetype == APR_DIR;
    ap_destroy_sub_req(sr);
  }

  /* now we need to truncate so we only have the directory */
  if (!dir && (last = ap_strrchr(rv, '/')) != NULL) {
    *((char*)last + 1) = '\0';
  }
  return rv;
}

/**
 * caches the compressed response for path @ compressed_path
 * @return 1 if compressed successfully, 0 otherwise
 */
static int ap_xsendfile_deflate(request_rec *r, const char *path, const char *compressed_path, int mode) {
#ifdef MOD_XSENDFILE_AUTO_GZIP
  const char *tmp_compress_path;

#ifndef HACKY_GZIP
  // zlib scratch variables
  int compression = -1;

  // cache compressed version of file into gzip format using zlib
  int ret, flush;
  unsigned have;
  zlib_context_t ctx;

  if (compression == -1) {
    compression = MOD_XSENDFILE_DEFLATE_DEFAULT_COMPRESSION_LEVEL;
  }

  /* allocate deflate state */
  ctx.strm.zalloc = Z_NULL;
  ctx.strm.zfree = Z_NULL;
  ctx.strm.opaque = Z_NULL;
  ret = deflateInit2(&strm, compression, Z_DEFLATED, MOD_XSENDFILE_ZLIB_WINDOWSIZE, MOD_XSENDFILE_ZLIB_CFACTOR, Z_DEFAULT_STRATEGY);

  if (ret != Z_OK) {
    return 0;
  }

  do {
    ctx.strm.avail_in = ;
  } while (flush != Z_FINISH);

  return 1;
#else /* HACKY_GZIP */
  const char *compress_cmd;
  pid_t child;
  pid_t wait_child;
  int wait_status;
  int outf;

  tmp_compress_path = apr_pstrcat(r->pool, compressed_path, ".tmp", NULL);

  outf = creat(tmp_compress_path, mode);
  if (outf == -1) {
    char *errmsg = strerror(errno);
    exit(1);
  }

  child = fork();
  if (child == -1) {
    return 0;
  }

  if (child == 0) {
    // child - exec the gzip command
    char *const argv[] = { "/bin/gzip", "--stdout", "-9", (char * const)path };

    if (-1 == dup2(outf, STDOUT_FILENO)) {
      exit(1);
    }

    execv("/bin/gzip", argv);
    // some error occured
    exit(1);
  }

  if (-1 == waitpid(child, &wait_status, 0)) {
    return 0;
  }

  if (!WIFEXITED(wait_status)) {
    return 0;
  } else {
    int exit_status = WEXITSTATUS(wait_status);
    // WTF: why does gzip exit with 1 when no error occured???
    if (exit_status != 0 && exit_status != 1) {
      return 0;
    }
  }

  if (0 != rename(tmp_compress_path, compressed_path)) {
    unlink(tmp_compress_path);
    return 0;
  }

  return 1;
#endif /* HACKY_GZIP */

#endif /* MOD_XSENDFILE_AUTO_GZIP */
}

static int ap_xsendfile_accepts_gzip(request_rec *r) {
  char *token;
  const char *accepts;

  /* Accept-Encoding logic copied from mod_deflate.c: */
  /* Even if we don't accept this request based on it not having
   * the Accept-Encoding, we need to note that we were looking
   * for this header and downstream proxies should be aware of that.
   */
  apr_table_mergen(r->headers_out, "Vary", "Accept-Encoding");

  accepts = apr_table_get(r->headers_in, "Accept-Encoding");
  if (accepts == NULL) {
    /* just pass-through the sendfile untouched */
    return 0;
  }
 
  token = ap_get_token(r->pool, &accepts, 0);
  while (token && token[0] && strcasecmp(token, "gzip")) {
    /* skip parameters, XXX: ;q=foo evaluation? */
    while (*accepts == ';') { 
      ++accepts;
      token = ap_get_token(r->pool, &accepts, 1);
    }
 
    /* retrieve next token */
    if (*accepts == ',') {
      ++accepts;
    }
    token = (*accepts) ? ap_get_token(r->pool, &accepts, 0) : NULL;
  }
  
  /* No acceptable token found. */
  if (token == NULL || token[0] == '\0') {
    return 0;
  }

  /* found gzip token */
  return 1;
}

static void ap_xsendfile_get_compressed_filepath(request_rec *r, /* out */ char **adjusted_path) {
  size_t pathlen;
  const char *path;
  char *deflate_path;
  struct stat original_stat;
  struct stat compressed_stat;

  path = *adjusted_path;

  if (!ap_xsendfile_accepts_gzip(r)) {
    return;
  }

  if (0 != stat(path, &original_stat)) {
#ifdef _DEBUG
    char errmsg[128];
    apr_strerror(apr_get_os_error(), errmsg, sizeof(errmsg) - 1);
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "xsendfile: can't stat %s: %s", path, errmsg);
#endif
    return;
  }

  deflate_path = apr_pstrcat(r->pool, path, ".gz", NULL);

  if (0 != stat(deflate_path, &compressed_stat) || compressed_stat.st_mtime < original_stat.st_mtime) {
#ifndef MOD_XSENDFILE_AUTO_GZIP
    // no zlib support so can't compress the file
#ifdef _DEBUG
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "xsendfile: zlib not available - can't compress %s", path);
#endif
    return;
#endif
    int compressible_path;
    int i;
    int mode = original_stat.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO);

    // TODO: make this configurable pattern-matching
    const char *compressible_extensions [] = {
      ".css",
      ".js",
      ".html",
      ".json",
    };

    size_t n_compressible_extensions = sizeof(compressible_extensions) / sizeof(compressible_extensions[0]);

    // compressed file doesn't exist or is older than the source file
    // check to make sure that it's compressible

    pathlen = strlen(path);

    compressible_path = 0;
    for (i = 0; i < n_compressible_extensions; i++) {
      const char *compressible_extension = compressible_extensions[i];
      size_t extension_length = strlen(compressible_extension);
      if (pathlen < extension_length) {
	continue;
      }

      if (strcmp(path + pathlen - extension_length, compressible_extension) == 0) {
	compressible_path = 1;
	break;
      }
    }

    if (!compressible_path) {
#ifdef _DEBUG
      ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "xsendfile: path %s doesn't have a compressible extension", path);
#endif
      return;
    }

    // do compression since file to serve is allowed to be compressible
    if (!ap_xsendfile_deflate(r, path, deflate_path, mode)) {
#ifdef _DEBUG
      ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "xsendfile: failed to compress %s to %s", path, deflate_path);
#endif
      return;
    }

    if (0 != stat(deflate_path, &compressed_stat)) {
#ifdef _DEBUG
      ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "xsendfile: failed to stat %s after compression succeeded?", deflate_path);
#endif
      return;
    }
  }

  {
    *adjusted_path = deflate_path;
    apr_table_set(r->headers_out, "Content-Length", apr_psprintf(r->pool, "%lu", (unsigned long)compressed_stat.st_size));
    apr_table_set(r->headers_out, "Content-Encoding", "gzip");
#ifdef _DEBUG
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "xsendfile: serving up encoded file %s", deflate_path);
#endif
  }

  return;
}

/*
  little helper function to build the file path if available
*/
static apr_status_t ap_xsendfile_get_filepath(request_rec *r,
    xsendfile_conf_t *conf, const char *file, int shouldDeleteFile,
    /* out */ char **path) {

  apr_status_t rv;

  apr_array_header_t *patharr;
  const xsendfile_path_t *paths;
  int i;

  patharr = conf->paths;
  if (!shouldDeleteFile) {
    const char *root = ap_xsendfile_get_orginal_path(r);
    if (root) {
      xsendfile_path_t *newpath;

#ifdef _DEBUG
      ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "xsendfile: path is %s", root);
#endif

      patharr = apr_array_make(
        r->pool,
        conf->paths->nelts + 1,
        sizeof(xsendfile_path_t)
        );
      newpath = apr_array_push(patharr);
      newpath->path = root;
      newpath->allowFileDelete = 0;
      apr_array_cat(patharr, conf->paths);
    }
  }

  if (patharr->nelts == 0) {
    return APR_EBADPATH;
  }

  paths = (const xsendfile_path_t*)patharr->elts;
  for (i = 0; i < patharr->nelts; ++i) {
    if (shouldDeleteFile && !paths[i].allowFileDelete){
      continue;
    }

    if ((rv = apr_filepath_merge(
      path,
      paths[i].path,
      file,
      APR_FILEPATH_TRUENAME | APR_FILEPATH_NOTABOVEROOT,
      r->pool
    )) == OK) {
#ifdef _DEBUG
      ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "xsendfile: finished merging at %d/%d elements", i, patharr->nelts);
#endif

      break;
    } else {
#ifdef _DEBUG
      ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "xsendfile: merged %d/%d elements (component = %s).  path is now %s", i, patharr->nelts, paths[i].path, *path);
#endif
    }
  }
  if (rv != OK) {
    *path = NULL;
  } else {
    ap_xsendfile_get_compressed_filepath(r, path);
  }
  return rv;
}

static apr_status_t ap_xsendfile_output_filter(ap_filter_t *f, apr_bucket_brigade *in) {
  request_rec *r = f->r, *sr = NULL;

  xsendfile_conf_t
    *dconf = ap_get_module_config(r->per_dir_config, &xsendfile_module),
    *sconf = ap_get_module_config(r->server->module_config, &xsendfile_module),
    *conf = xsendfile_config_merge(r->pool, sconf, dconf);

  core_dir_config *coreconf = ap_get_module_config(r->per_dir_config, &core_module);

  apr_status_t rv;
  apr_bucket *e;

  apr_file_t *fd = NULL;
  apr_finfo_t finfo;

  char *file = NULL;
  char *translated = NULL;
  char *translatedEncoding = NULL;

  int errcode;
  int shouldDeleteFile = 0;

#ifdef _DEBUG
  ap_log_error(
    APLOG_MARK,
    APLOG_DEBUG,
    0,
    r->server,
    "xsendfile: output_filter for %s",
    r->the_request
    );
#endif
  /*
    should we proceed with this request?

    * sub-requests suck
    * furthermore default-handled requests suck, as they actually shouldn't be able to set headers
  */
  if (
    r->status != HTTP_OK
    || r->main
    || (r->handler && strcmp(r->handler, "default-handler") == 0)
  ) {
#ifdef _DEBUG
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "xsendfile: not met [%d]", r->status);
#endif
    ap_remove_output_filter(f);
    return ap_pass_brigade(f->next, in);
  }

  /*
    alright, look for x-sendfile
  */
  file = (char*)apr_table_get(r->headers_out, AP_XSENDFILE_HEADER);

  /* cgi/fastcgi will put the stuff into err_headers_out */
  if (!file || !*file) {
    file = (char*)apr_table_get(r->err_headers_out, AP_XSENDFILE_HEADER);
  }

  /*
    so...there is no X-SendFile header, check if there is an X-Sendfile-Temporary header
  */
  if (!file || !*file) {
    shouldDeleteFile = 1;
    file = (char*)apr_table_get(r->headers_out, AP_XSENDFILETEMPORARY_HEADER);
  }
  /*
    Maybe X-Sendfile-Temporary is set via cgi in error_headers_out?
  */
  if (!file || !*file) {
    file = (char*)apr_table_get(r->err_headers_out, AP_XSENDFILETEMPORARY_HEADER);
  }

  /* Remove any X-Sendfile headers */
  apr_table_unset(r->headers_out, AP_XSENDFILE_HEADER);
  apr_table_unset(r->err_headers_out, AP_XSENDFILE_HEADER);
  apr_table_unset(r->headers_out, AP_XSENDFILETEMPORARY_HEADER);
  apr_table_unset(r->err_headers_out, AP_XSENDFILETEMPORARY_HEADER);

  /* nothing there :p */
  if (!file || !*file) {
#ifdef _DEBUG
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "xsendfile: nothing found");
#endif
    ap_remove_output_filter(f);
    return ap_pass_brigade(f->next, in);
  }

  /*
    drop *everything*
    might be pretty expensive to generate content first that goes straight to the bitbucket,
    but actually the scripts that might set this flag won't output too much anyway
  */
  while (!APR_BRIGADE_EMPTY(in)) {
    e = APR_BRIGADE_FIRST(in);
    apr_bucket_delete(e);
  }
  r->eos_sent = 0;

  /* as we dropped all the content this field is not valid anymore! */
  apr_table_unset(r->headers_out, "Content-Length");
  apr_table_unset(r->err_headers_out, "Content-Length");
  apr_table_unset(r->headers_out, "Content-Encoding");
  apr_table_unset(r->err_headers_out, "Content-Encoding");

  /* Decode header
     lighttpd does the same for X-Sendfile2, so we're compatible here
     */
  if (conf->unescape != XSENDFILE_DISABLED) {
    rv = ap_unescape_url(file);
    if (rv != OK) {
      /* Unescaping failed, probably due to bad encoding.
         Note that NOT_FOUND refers to escape sequences containing slashes,
         what we do not allow (use real slashes only)
         */
      ap_log_rerror(
        APLOG_MARK,
        APLOG_ERR,
        rv,
        r,
        "xsendfile: bad file name encoding"
        );
      ap_remove_output_filter(f);
      ap_die(HTTP_INTERNAL_SERVER_ERROR, r);
      return HTTP_INTERNAL_SERVER_ERROR;
    }
  }

  /* lookup/verification of the given path */
  rv = ap_xsendfile_get_filepath(
    r,
    conf,
    file,
    shouldDeleteFile,
    &translated
    );
  if (rv != OK) {
    ap_log_rerror(
      APLOG_MARK,
      APLOG_ERR,
      rv,
      r,
      "xsendfile: unable to find file: %s",
      file
      );
    ap_remove_output_filter(f);
    ap_die(HTTP_NOT_FOUND, r);
    return HTTP_NOT_FOUND;
  }

  /*
    try open the file
  */
  if ((rv = apr_file_open(
    &fd,
    translated,
    APR_READ | APR_BINARY
    | (shouldDeleteFile ? APR_DELONCLOSE : 0)  /* if this is a temporary file, delete on close */
#if APR_HAS_SENDFILE
    | (coreconf->enable_sendfile != ENABLE_SENDFILE_OFF ? APR_SENDFILE_ENABLED : 0)
#endif
    ,
    0,
    r->pool
  )) != APR_SUCCESS) {
    ap_log_rerror(
      APLOG_MARK,
      APLOG_ERR,
      rv,
      r,
      "xsendfile: cannot open file: %s",
      translated
      );
    ap_remove_output_filter(f);
    ap_die(HTTP_NOT_FOUND, r);
    return HTTP_NOT_FOUND;
  }
#if APR_HAS_SENDFILE && defined(_DEBUG)
  if (coreconf->enable_sendfile == ENABLE_SENDFILE_OFF) {
    ap_log_error(
      APLOG_MARK,
      APLOG_WARNING,
      0,
      r->server,
      "xsendfile: sendfile configured, but not active %d",
      coreconf->enable_sendfile
      );
    }
#endif
  /* stat (for etag/cache/content-length stuff) */
  if ((rv = apr_file_info_get(&finfo, APR_FINFO_NORM, fd)) != APR_SUCCESS) {
    ap_log_rerror(
      APLOG_MARK,
      APLOG_ERR,
      rv,
      r,
      "xsendfile: unable to stat file: %s",
      translated
      );
    apr_file_close(fd);
    ap_remove_output_filter(f);
    ap_die(HTTP_FORBIDDEN, r);
    return HTTP_FORBIDDEN;
  }
  /* no inclusion of directories! we're serving files! */
  if (finfo.filetype != APR_REG) {
    ap_log_rerror(
      APLOG_MARK,
      APLOG_ERR,
      APR_EBADPATH,
      r,
      "xsendfile: not a file %s",
      translated
      );
    apr_file_close(fd);
    ap_remove_output_filter(f);
    ap_die(HTTP_NOT_FOUND, r);
    return HTTP_NOT_FOUND;
  }

  /*
    need to cheat here a bit
    as etag generator will use those ;)
    and we want local_copy and cache
  */
  r->finfo.inode = finfo.inode;
  r->finfo.size = finfo.size;

  /*
    caching? why not :p
  */
  r->no_cache = r->no_local_copy = 0;

  /* some script (f?cgi) place stuff in err_headers_out */
  if (
    conf->ignoreLM == XSENDFILE_ENABLED
    || (
      !apr_table_get(r->headers_out, "last-modified")
      && !apr_table_get(r->err_headers_out, "last-modified")
    )
  ) {
    apr_table_unset(r->err_headers_out, "last-modified");
    ap_update_mtime(r, finfo.mtime);
    ap_set_last_modified(r);
  }
  if (
    conf->ignoreETag == XSENDFILE_ENABLED
    || (
      !apr_table_get(r->headers_out, "etag")
      && !apr_table_get(r->err_headers_out, "etag")
    )
  ) {
    apr_table_unset(r->err_headers_out, "etag");
    ap_set_etag(r);
  }

  ap_set_content_length(r, finfo.size);

  /* cache or something? */
  if ((errcode = ap_meets_conditions(r)) != OK) {
#ifdef _DEBUG
    ap_log_error(
      APLOG_MARK,
      APLOG_DEBUG,
      0,
      r->server,
      "xsendfile: met condition %d for %s",
      errcode,
      file
      );
#endif
    apr_file_close(fd);
    r->status = errcode;
  }
  else {
    /* For platforms where the size of the file may be larger than
     * that which can be stored in a single bucket (where the
     * length field is an apr_size_t), split it into several
     * buckets: */
    if (sizeof(apr_off_t) > sizeof(apr_size_t)
      && finfo.size > AP_MAX_SENDFILE) {
      apr_off_t fsize = finfo.size;
      e = apr_bucket_file_create(fd, 0, AP_MAX_SENDFILE, r->pool,
                                 in->bucket_alloc);
      while (fsize > AP_MAX_SENDFILE) {
          apr_bucket *ce;
          apr_bucket_copy(e, &ce);
          APR_BRIGADE_INSERT_TAIL(in, ce);
          e->start += AP_MAX_SENDFILE;
          fsize -= AP_MAX_SENDFILE;
      }
      e->length = (apr_size_t)fsize; /* Resize just the last bucket */
    }
    else {
      e = apr_bucket_file_create(fd, 0, (apr_size_t)finfo.size,
                                 r->pool, in->bucket_alloc);
    }


#if APR_HAS_MMAP
    if (coreconf->enable_mmap == ENABLE_MMAP_ON) {
      apr_bucket_file_enable_mmap(e, 0);
    }
#if defined(_DEBUG)
    else {
      ap_log_error(
        APLOG_MARK,
        APLOG_WARNING,
        0,
        r->server,
        "xsendfile: mmap configured, but not active %d",
        coreconf->enable_mmap
        );
      }
#endif /* _DEBUG */
#endif /* APR_HAS_MMAP */
    APR_BRIGADE_INSERT_TAIL(in, e);
  }

  e = apr_bucket_eos_create(in->bucket_alloc);
  APR_BRIGADE_INSERT_TAIL(in, e);

  /* remove ourselves from the filter chain */
  ap_remove_output_filter(f);

#ifdef _DEBUG
  ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "xsendfile: sending %d bytes", (int)finfo.size);
#endif

  /* send the data up the stack */
  return ap_pass_brigade(f->next, in);
}

static void ap_xsendfile_insert_output_filter(request_rec *r) {
  xsendfile_conf_active_t enabled = ((xsendfile_conf_t *)ap_get_module_config(r->per_dir_config, &xsendfile_module))->enabled;
  if (XSENDFILE_UNSET == enabled) {
    enabled = ((xsendfile_conf_t*)ap_get_module_config(r->server->module_config, &xsendfile_module))->enabled;
  }

  if (XSENDFILE_ENABLED != enabled) {
    return;
  }

  ap_add_output_filter(
    "XSENDFILE",
    NULL,
    r,
    r->connection
	  );
}
static const command_rec xsendfile_command_table[] = {
  AP_INIT_FLAG(
    "XSendFile",
    xsendfile_cmd_flag,
    NULL,
    OR_FILEINFO,
    "On|Off - Enable/disable(default) processing"
    ),
  AP_INIT_FLAG(
    "XSendFileIgnoreEtag",
    xsendfile_cmd_flag,
    NULL,
    OR_FILEINFO,
    "On|Off - Ignore script provided Etag headers (default: Off)"
    ),
  AP_INIT_FLAG(
    "XSendFileIgnoreLastModified",
    xsendfile_cmd_flag,
    NULL,
    OR_FILEINFO,
    "On|Off - Ignore script provided Last-Modified headers (default: Off)"
    ),
  AP_INIT_FLAG(
    "XSendFileUnescape",
    xsendfile_cmd_flag,
    NULL,
    OR_FILEINFO,
    "On|Off - Unescape/url-decode the value of the header (default: On)"
    ),
  AP_INIT_TAKE12(
    "XSendFilePath",
    xsendfile_cmd_path,
    NULL,
    RSRC_CONF|ACCESS_CONF,
    "Allow to serve files from that Path. Must be absolute"
    ),
  { NULL }
};
static void xsendfile_register_hooks(apr_pool_t *p) {
  ap_register_output_filter(
    "XSENDFILE",
    ap_xsendfile_output_filter,
    NULL,
    AP_FTYPE_CONTENT_SET
    );

  ap_hook_insert_filter(
    ap_xsendfile_insert_output_filter,
    NULL,
    NULL,
    APR_HOOK_LAST + 1
    );
}
module AP_MODULE_DECLARE_DATA xsendfile_module = {
  STANDARD20_MODULE_STUFF,
  xsendfile_config_perdir_create,
  xsendfile_config_merge,
  xsendfile_config_server_create,
  xsendfile_config_merge,
  xsendfile_command_table,
  xsendfile_register_hooks
};

/* vim: set ts=2 sw=2 et : */
