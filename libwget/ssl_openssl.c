/*
 * Copyright (c) 2015-2021 Free Software Foundation, Inc.
 *
 * This file is part of libwget.
 *
 * Libwget is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Libwget is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libwget.  If not, see <https://www.gnu.org/licenses/>.
 *
 *
 * SSL/TLS routines, with OpenSSL as the backend engine
 *
 * Author: Ander Juaristi
 */

#include <config.h>

#include <dirent.h>
#include <limits.h> // INT_MAX
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/ocsp.h>
#include <openssl/crypto.h>
#include <openssl/ossl_typ.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#  include <w32sock.h>
#else
#  define FD_TO_SOCKET(x) (x)
#  define SOCKET_TO_FD(x) (x)
#endif

#ifdef LIBRESSL_VERSION_NUMBER
  #ifndef TLS_MAX_VERSION
    #ifndef TLS1_3_VERSION
      #define TLS1_3_VERSION TLS1_2_VERSION
      #define TLS_MAX_VERSION TLS1_2_VERSION
    #else
      #define TLS_MAX_VERSION TLS1_3_VERSION
    #endif
  #endif
#endif

#include <wget.h>
#include "private.h"
#include "net.h"

static wget_tls_stats_callback
	*tls_stats_callback;
static void
	*tls_stats_ctx;

static wget_ocsp_stats_callback
	*ocsp_stats_callback;
static void
	*ocsp_stats_ctx;

static struct config
{
	const char
		*secure_protocol,
		*ca_directory,
		*ca_file,
		*cert_file,
		*key_file,
		*crl_file,
		*ocsp_server,
		*alpn;
	wget_ocsp_db
		*ocsp_cert_cache,
		*ocsp_host_cache;
	wget_tls_session_db
		*tls_session_cache;
	wget_hpkp_db
		*hpkp_cache;
	char
		ca_type,
		cert_type,
		key_type;
	bool
		check_certificate :1,
		check_hostname :1,
		print_info :1,
		ocsp :1,
		ocsp_date :1,
		ocsp_stapling :1,
		ocsp_nonce :1;
} config = {
	.check_certificate = 1,
	.check_hostname = 1,
#ifdef WITH_OCSP
	.ocsp = 1,
	.ocsp_stapling = 1,
#endif
	.ca_type = WGET_SSL_X509_FMT_PEM,
	.cert_type = WGET_SSL_X509_FMT_PEM,
	.key_type = WGET_SSL_X509_FMT_PEM,
	.secure_protocol = "AUTO",
	.ca_directory = "system",
#ifdef WITH_LIBNGHTTP2
	.alpn = "h2,http/1.1"
#endif
	};

static int init;
static wget_thread_mutex mutex;

static SSL_CTX *_ctx;
static int store_userdata_idx;

/*
 * Constructor & destructor
 */
static void __attribute__ ((constructor)) tls_init(void)
{
	if (!mutex)
		wget_thread_mutex_init(&mutex);

	store_userdata_idx = X509_STORE_CTX_get_ex_new_index(
			0, NULL,	/* argl, argp */
			NULL,		/* new_func */
			NULL,		/* dup_func */
			NULL);		/* free_func */
}

static void __attribute__ ((destructor)) tls_exit(void)
{
	if (mutex)
		wget_thread_mutex_destroy(&mutex);
}

/*
 * SSL/TLS configuration functions
 */

/**
 * \param[in] key An identifier for the config parameter (starting with `WGET_SSL_`) to set
 * \param[in] value The value for the config parameter (a NULL-terminated string)
 *
 * Set a configuration parameter, as a string.
 *
 * The following parameters accept a string as their value (\p key can have any of those values):
 *
 *  - WGET_SSL_SECURE_PROTOCOL: A string describing which SSL/TLS version should be used. It can have either
 *  an arbitrary value, or one of the following fixed values (case does not matter):
 *      - "SSL": SSLv3 will be used. Warning: this protocol is insecure and should be avoided.
 *      - "TLSv1": TLS 1.0 will be used.
 *      - "TLSv1_1": TLS 1.1 will be used.
 *      - "TLSv1_2": TLS 1.2 will be used.
 *      - "TLSv1_3": TLS 1.3 will be used.
 *      - "AUTO": Let the TLS library decide.
 *      - "PFS": Let the TLS library decide, but make sure only forward-secret ciphers are used.
 *
 *  An arbitrary string can also be supplied (an string that's different from any of the previous ones). If that's the case
 *  the string will be directly taken as the priority string and sent to the library. Priority strings provide the greatest flexibility,
 *  but have a library-specific syntax. A GnuTLS priority string will not work if your libwget has been compiled with OpenSSL, for instance.
 *  - WGET_SSL_CA_DIRECTORY: A path to the directory where the root certificates will be taken from
 *  for server cert validation. Every file of that directory is expected to contain an X.509 certificate,
 *  encoded in PEM format. If the string "system" is specified, the system's default directory will be used.
 *  The default value is "system". Certificates get loaded in wget_ssl_init().
 *  - WGET_SSL_CA_FILE: A path to a file containing a single root certificate. This will be used to validate
 *  the server's certificate chain. This option can be used together with `WGET_SSL_CA_DIRECTORY`. The certificate
 *  can be in either PEM or DER format. The format is specified in the `WGET_SSL_CA_TYPE` option (see
 *  wget_ssl_set_config_int()).
 *  - WGET_SSL_CERT_FILE: Set the client certificate. It will be used for client authentication if the server requests it.
 *  It can be in either PEM or DER format. The format is specified in the `WGET_SSL_CERT_TYPE` option (see
 *  wget_ssl_set_config_int()). The `WGET_SSL_KEY_FILE` option specifies the private key corresponding to the cert's
 *  public key. If `WGET_SSL_KEY_FILE` is not set, then the private key is expected to be in the same file as the certificate.
 *  - WGET_SSL_KEY_FILE: Set the private key corresponding to the client certificate specified in `WGET_SSL_CERT_FILE`.
 *  It can be in either PEM or DER format. The format is specified in the `WGET_SSL_KEY_TYPE` option (see
 *  wget_ssl_set_config_int()). IF `WGET_SSL_CERT_FILE` is not set, then the certificate is expected to be in the same file
 *  as the private key.
 *  - WGET_SSL_CRL_FILE: Sets a CRL (Certificate Revocation List) file which will be used to verify client and server certificates.
 *  A CRL file is a black list that contains the serial numbers of the certificates that should not be treated as valid. Whenever
 *  a client or a server presents a certificate in the TLS handshake whose serial number is contained in the CRL, the handshake
 *  will be immediately aborted. The CRL file must be in PEM format.
 *  - WGET_SSL_OCSP_SERVER: Set the URL of the OCSP server that will be used to validate certificates.
 *  OCSP is a protocol by which a server is queried to tell whether a given certificate is valid or not. It's an approach contrary
 *  to that used by CRLs. While CRLs are black lists, OCSP takes a white list approach where a certificate can be checked for validity.
 *  Whenever a client or server presents a certificate in a TLS handshake, the provided URL will be queried (using OCSP) to check whether
 *  that certificate is valid or not. If the server responds the certificate is not valid, the handshake will be immediately aborted.
 *  - WGET_SSL_ALPN: Sets the ALPN string to be sent to the remote host. ALPN is a TLS extension
 *  ([RFC 7301](https://tools.ietf.org/html/rfc7301))
 *  that allows both the server and the client to signal which application-layer protocols they support (HTTP/2, QUIC, etc.).
 *  That information can then be used for the server to ultimately decide which protocol will be used on top of TLS.
 *
 *  An invalid value for \p key will not harm the operation of TLS, but will cause
 *  a complain message to be printed to the error log stream.
 */
void wget_ssl_set_config_string(int key, const char *value)
{
	switch (key) {
	case WGET_SSL_SECURE_PROTOCOL:
		config.secure_protocol = value;
		break;
	case WGET_SSL_CA_DIRECTORY:
		config.ca_directory = value;
		break;
	case WGET_SSL_CA_FILE:
		config.ca_file = value;
		break;
	case WGET_SSL_CERT_FILE:
		config.cert_file = value;
		break;
	case WGET_SSL_KEY_FILE:
		config.key_file = value;
		break;
	case WGET_SSL_CRL_FILE:
		config.crl_file = value;
		break;
	case WGET_SSL_OCSP_SERVER:
		config.ocsp_server = value;
		break;
	case WGET_SSL_ALPN:
		config.alpn = value;
		break;
	default:
		error_printf(_("Unknown configuration key %d (maybe this config value should be of another type?)\n"), key);
	}
}

/**
 * \param[in] key An identifier for the config parameter (starting with `WGET_SSL_`) to set
 * \param[in] value The value for the config parameter (a pointer)
 *
 * Set a configuration parameter, as a libwget object.
 *
 * The following parameters expect an already initialized libwget object as their value.
 *
 * - WGET_SSL_OCSP_CACHE: This option takes a pointer to a \ref wget_ocsp_db
 *  structure as an argument. Such a pointer is returned when initializing the OCSP cache with wget_ocsp_db_init().
 *  The cache is used to store OCSP responses locally and avoid querying the OCSP server repeatedly for the same certificate.
 *  - WGET_SSL_SESSION_CACHE: This option takes a pointer to a \ref wget_tls_session_db structure.
 *  Such a pointer is returned when initializing the TLS session cache with wget_tls_session_db_init().
 *  This option thus sets the handle to the TLS session cache that will be used to store TLS sessions.
 *  The TLS session cache is used to support TLS session resumption. It stores the TLS session parameters derived from a previous TLS handshake
 *  (most importantly the session identifier and the master secret) so that there's no need to run the handshake again
 *  the next time we connect to the same host. This is useful as the handshake is an expensive process.
 *  - WGET_SSL_HPKP_CACHE: Set the HPKP cache to be used to verify known HPKP pinned hosts. This option takes a pointer
 *  to a \ref wget_hpkp_db structure. Such a pointer is returned when initializing the HPKP cache
 *  with wget_hpkp_db_init(). HPKP is a HTTP-level protocol that allows the server to "pin" its present and future X.509
 *  certificate fingerprints, to support rapid certificate change in the event that the higher level root CA
 *  gets compromised ([RFC 7469](https://tools.ietf.org/html/rfc7469)).
 */
void wget_ssl_set_config_object(int key, void *value)
{
	switch (key) {
	case WGET_SSL_OCSP_CACHE:
		config.ocsp_cert_cache = (wget_ocsp_db *) value;
		break;
	case WGET_SSL_SESSION_CACHE:
		config.tls_session_cache = (wget_tls_session_db *) value;
		break;
	case WGET_SSL_HPKP_CACHE:
		config.hpkp_cache = (wget_hpkp_db *) value;
		break;
	default:
		error_printf(_("Unknown configuration key %d (maybe this config value should be of another type?)\n"), key);
	}
}

/**
 * \param[in] key An identifier for the config parameter (starting with `WGET_SSL_`)
 * \param[in] value The value for the config parameter
 *
 * Set a configuration parameter, as an integer.
 *
 * These are the parameters that can be set (\p key can have any of these values):
 *
 *  - WGET_SSL_CHECK_CERTIFICATE: whether certificates should be verified (1) or not (0)
 *  - WGET_SSL_CHECK_HOSTNAME: whether or not to check if the certificate's subject field
 *  matches the peer's hostname. This check is done according to the rules in [RFC 6125](https://tools.ietf.org/html/rfc6125)
 *  and typically involves checking whether the hostname and the common name (CN) field of the subject match.
 *  - WGET_SSL_PRINT_INFO: whether or not information should be printed about the established SSL/TLS handshake (negotiated
 *  ciphersuites, certificates, etc.). The default is no (0).
 *
 * The following three options all can take either `WGET_SSL_X509_FMT_PEM` (to specify the PEM format) or `WGET_SSL_X509_FMT_DER`
 * (for the DER format). The default in for all of them is `WGET_SSL_X509_FMT_PEM`.
 *
 *  - WGET_SSL_CA_TYPE: Specifies what's the format of the root CA certificate(s) supplied with either `WGET_SSL_CA_DIRECTORY`
 *  or `WGET_SSL_CA_FILE`.
 *  - WGET_SSL_CERT_TYPE: Specifies what's the format of the certificate file supplied with `WGET_SSL_CERT_FILE`. **The certificate
 *  and the private key supplied must both be of the same format.**
 *  - WGET_SSL_KEY_TYPE: Specifies what's the format of the private key file supplied with `WGET_SSL_KEY_FILE`. **The private key
 *  and the certificate supplied must both be of the same format.**
 *
 * The following two options control OCSP queries. These don't affect the CRL set with `WGET_SSL_CRL_FILE`, if any.
 * If both CRLs and OCSP are enabled, both will be used.
 *
 *  - WGET_SSL_OCSP: whether or not OCSP should be used. The default is yes (1).
 *  - WGET_SSL_OCSP_STAPLING: whether or not OCSP stapling should be used. The default is yes (1).
 *  - WGET_SSL_OCSP_NONCE: whether or not an OCSP nonce should be sent in the request. The default is yes (1).
 *  If a nonce was sent in the request, the OCSP verification will fail if the response nonce doesn't match.
 *  However if the response does not include a nonce extension, verification will be allowed to continue.
 *  The OCSP nonce extension is not a critical one.
 *  - WGET_SSL_OCSP_DATE: Reject the OCSP response if it's older than 3 days.
 */
void wget_ssl_set_config_int(int key, int value)
{
	switch (key) {
	case WGET_SSL_CHECK_CERTIFICATE:
		config.check_certificate = value;
		break;
	case WGET_SSL_CHECK_HOSTNAME:
		config.check_hostname = value;
		break;
	case WGET_SSL_PRINT_INFO:
		config.print_info = value;
		break;
	case WGET_SSL_CA_TYPE:
		config.ca_type = (char) value;
		break;
	case WGET_SSL_CERT_TYPE:
		config.cert_type = (char) value;
		break;
	case WGET_SSL_KEY_TYPE:
		config.key_type = (char) value;
		break;
	case WGET_SSL_OCSP:
		config.ocsp = value;
		break;
	case WGET_SSL_OCSP_STAPLING:
		config.ocsp_stapling = value;
		break;
	case WGET_SSL_OCSP_NONCE:
		config.ocsp_nonce = value;
		break;
	case WGET_SSL_OCSP_DATE:
		config.ocsp_date = value;
		break;
	default:
		error_printf(_("Unknown configuration key %d (maybe this config value should be of another type?)\n"), key);
	}
}

/*
 * SSL/TLS core public API
 */
static int openssl_load_crl(X509_STORE *store, const char *crl_file)
{
	X509_LOOKUP *lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file());

	if (!X509_load_crl_file(lookup, crl_file, X509_FILETYPE_PEM))
		return WGET_E_UNKNOWN;
	if (!X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL | X509_V_FLAG_USE_DELTAS))
		return WGET_E_UNKNOWN;

	return 0;
}

#define SET_MIN_VERSION(ctx, ver) \
	if (!SSL_CTX_set_min_proto_version(ctx, ver)) \
		return WGET_E_UNKNOWN

static int openssl_set_priorities(SSL_CTX *ctx, const char *prio)
{
	/*
	 * Default ciphers. This is what will be used
	 * if 'auto' is specified as the priority (currently the default).
	 */
	const char *openssl_ciphers = "HIGH:!aNULL:!RC4:!MD5:!SRP:!PSK";

	SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
	SSL_CTX_set_max_proto_version(ctx, TLS_MAX_VERSION);

	if (!wget_strcasecmp_ascii(prio, "SSL")) {
		SET_MIN_VERSION(ctx, SSL3_VERSION);
	} else if (!wget_strcasecmp_ascii(prio, "TLSv1")) {
		SET_MIN_VERSION(ctx, TLS1_VERSION);
	} else if (!wget_strcasecmp_ascii(prio, "TLSv1_1")) {
		SET_MIN_VERSION(ctx, TLS1_1_VERSION);
	/*
	 * Skipping "TLSv1_2".
	 * Checking for "TLSv1_2" is totally redundant - we already set it as the minimum supported version by default
	 */
	} else if (!wget_strcasecmp_ascii(prio, "TLSv1_3")) {
		/* OpenSSL supports TLS 1.3 starting at 1.1.1-beta9 (0x10101009) */
#if OPENSSL_VERSION_NUMBER >= 0x10101009
		SET_MIN_VERSION(ctx, TLS1_3_VERSION);
#else
		info_printf(_("OpenSSL: TLS 1.3 is not supported by your OpenSSL version. Will use TLS 1.2 instead.\n"));
#endif
	} else if (!wget_strcasecmp_ascii(prio, "PFS")) {
		/* Forward-secrecy - Disable RSA key exchange! */
		openssl_ciphers = "HIGH:!aNULL:!RC4:!MD5:!SRP:!PSK:!kRSA";
	} else if (prio && wget_strcasecmp_ascii(prio, "AUTO") && wget_strcasecmp_ascii(prio, "TLSv1_2")) {
		openssl_ciphers = prio;
	}

	if (!SSL_CTX_set_cipher_list(ctx, openssl_ciphers)) {
		error_printf(_("OpenSSL: Invalid priority string '%s'\n"), prio);
		return WGET_E_INVALID;
	}

	return 0;
}

static int openssl_load_trust_file(SSL_CTX *ctx, const char *dir, const char *file)
{
	char sbuf[256];
	wget_buffer buf;
	int rc;

	wget_buffer_init(&buf, sbuf, sizeof(sbuf));

	wget_buffer_printf(&buf, "%s/%s", dir, file);
	rc = (SSL_CTX_load_verify_locations(ctx, buf.data, NULL) ? 0 : -1);

	wget_buffer_deinit(&buf);

	return rc;
}

static int openssl_load_trust_files_from_directory(SSL_CTX *ctx, const char *dirname)
{
	DIR *dir;
	struct dirent *dp;
	int loaded = 0;

	if ((dir = opendir(dirname))) {
		while ((dp = readdir(dir))) {
			if (*dp->d_name == '.')
				continue;

			if (wget_match_tail_nocase(dp->d_name, ".pem")
				&& openssl_load_trust_file(ctx, dirname, dp->d_name) == 0)
			{
				loaded++;
			}
		}

		closedir(dir);
	}

	return loaded;
}

static int openssl_load_trust_files(SSL_CTX *ctx, const char *dir)
{
	int retval;

	if (!strcmp(dir, "system")) {
		/*
		 * Load system-provided certificates.
		 * Either "/etc/ssl/certs" or OpenSSL's default (if provided).
		 */
		if (SSL_CTX_set_default_verify_paths(ctx)) {
			retval = 0;
			goto end;
		}

		dir = "/etc/ssl/certs";
		info_printf(_("OpenSSL: Could not load certificates from default paths. Falling back to '%s'."), dir);
	}

	retval = openssl_load_trust_files_from_directory(ctx, dir);
	if (retval == 0)
		error_printf(_("OpenSSL: No certificates could be loaded from directory '%s'\n"), dir);
	else if (retval > 0)
		debug_printf("OpenSSL: Loaded %d certificates\n", retval);
	else
		error_printf(_("OpenSSL: Could not open directory '%s'. No certificates were loaded.\n"), dir);

end:
	return retval;
}

static int verify_hpkp(const char *hostname, X509 *subject_cert, wget_hpkp_stats_result *hpkp_stats)
{
	int retval, spki_len;
	unsigned char *spki = NULL;

	/* Get certificate's public key in DER format */
	spki_len = i2d_PUBKEY(X509_get0_pubkey(subject_cert), &spki);
	if (spki_len <= 0)
		return -1;

	/* Lookup database */
	retval = wget_hpkp_db_check_pubkey(config.hpkp_cache,
		hostname,
		spki, spki_len);

	switch (retval) {
	case 1:
		debug_printf("Matching HPKP pinning found for host '%s'\n", hostname);
		*hpkp_stats = WGET_STATS_HPKP_MATCH;
		retval = 0;
		break;
	case 0:
		debug_printf("No HPKP pinning found for host '%s'\n", hostname);
		*hpkp_stats = WGET_STATS_HPKP_NO;
		retval = 1;
		break;
	case -2:
		debug_printf("Public key for host '%s' does not match\n", hostname);
		*hpkp_stats = WGET_STATS_HPKP_NOMATCH;
		retval = -1;
		break;
	default:
		debug_printf("Could not check HPKP pinning for host '%s' (%d)\n", hostname, retval);
		*hpkp_stats = WGET_STATS_HPKP_ERROR;
		retval = 0;
	}

	OPENSSL_free(spki);
	return retval;
}

static int check_cert_chain_for_hpkp(STACK_OF(X509) *certs, const char *hostname, wget_hpkp_stats_result *hpkp_stats)
{
	int retval, pin_ok = 0;
	X509 *cert;
	unsigned cert_list_size = sk_X509_num(certs);

	for (unsigned i = 0; i < cert_list_size; i++) {
		cert = sk_X509_value(certs, i);

		if ((retval = verify_hpkp(hostname, cert, hpkp_stats)) >= 0)
			pin_ok = 1;
		if (retval == 1)
			break;
	}

	return pin_ok;
}

struct verification_flags {
	SSL
		*ssl;
	const char
		*hostname;
	X509_STORE
		*certstore;
	unsigned int
		cert_chain_size;
	wget_hpkp_stats_result
		hpkp_stats;
	bool
		verifying_ocsp,
		ocsp_checked;
};

static int check_ocsp_response(OCSP_RESPONSE *,
		STACK_OF(X509) *,
		X509_STORE *,
		bool);

static int ocsp_resp_cb(SSL *s, void *arg)
{
	int result;
	long ocsp_resp_len;
	const unsigned char *ocsp_resp_raw = NULL;
	OCSP_RESPONSE *ocspresp;
	STACK_OF(X509) *certstack;
	struct verification_flags *vflags = arg;

	if (!vflags)
		return 0;

	ocsp_resp_len = SSL_get_tlsext_status_ocsp_resp(s, &ocsp_resp_raw);
	if (ocsp_resp_len == -1) {
		debug_printf("No stapled OCSP response was received. Continuing.\n");
		return 1;
	}

	ocspresp = d2i_OCSP_RESPONSE(NULL, &ocsp_resp_raw, ocsp_resp_len);
	if (!ocspresp) {
		error_printf(_("Got a stapled OCSP response, but could not parse it. Aborting.\n"));
		return 0;
	}

	certstack = SSL_get_peer_cert_chain(vflags->ssl);
	if (!certstack) {
		error_printf(_("Could not get server's cert stack\n"));
		return 0;
	}

	result = check_ocsp_response(ocspresp,
		certstack,
		vflags->certstore,
		config.ocsp_date); /* check time? */

	if (result == -1) {
		OCSP_RESPONSE_free(ocspresp);
		error_printf(_("Could not verify stapled OCSP response. Aborting.\n"));
		return 0;
	}

	OCSP_RESPONSE_free(ocspresp);
	debug_printf("Got a stapled OCSP response. Length: %ld. Status: OK\n", ocsp_resp_len);
	return 1;
}

static OCSP_REQUEST *send_ocsp_request(const char *uri,
		OCSP_CERTID *certid,
		wget_http_response **response)
{
	OCSP_REQUEST *ocspreq;
	wget_http_response *resp;
	unsigned char *ocspreq_bytes = NULL;
	size_t ocspreq_bytes_len;

	ocspreq = OCSP_REQUEST_new();
	if (!ocspreq)
		goto end;

	if (!OCSP_request_add0_id(ocspreq, certid)) {
		OCSP_REQUEST_free(ocspreq);
		ocspreq = NULL;
		goto end;
	}

	if (config.ocsp_nonce && !OCSP_request_add1_nonce(ocspreq, NULL, 0)) {
		OCSP_REQUEST_free(ocspreq);
		ocspreq = NULL;
		goto end;
	}

	ocspreq_bytes_len = i2d_OCSP_REQUEST(ocspreq, &ocspreq_bytes);
	if (!ocspreq_bytes || !ocspreq_bytes_len) {
		OCSP_REQUEST_free(ocspreq);
		ocspreq = NULL;
		goto end;
	}

	resp = wget_http_get(
		WGET_HTTP_URL, uri,
		WGET_HTTP_SCHEME, "POST",
		WGET_HTTP_HEADER_ADD, "Accept-Encoding", "identity",
		WGET_HTTP_HEADER_ADD, "Accept", "application/ocsp-response",
		WGET_HTTP_HEADER_ADD, "Content-Type", "application/ocsp-request",
		WGET_HTTP_MAX_REDIRECTIONS, 5,
		WGET_HTTP_BODY, ocspreq_bytes, ocspreq_bytes_len,
		0);

	OPENSSL_free(ocspreq_bytes);

	if (resp) {
		*response = resp;
	} else {
		OCSP_REQUEST_free(ocspreq);
		ocspreq = NULL;
	}

end:
	return ocspreq;
}

static const char *get_printable_ocsp_reason_desc(int reason)
{
	switch (reason) {
	case OCSP_REVOKED_STATUS_NOSTATUS:
		return "not given";
	case OCSP_REVOKED_STATUS_UNSPECIFIED:
		return "unspecified";
	case OCSP_REVOKED_STATUS_KEYCOMPROMISE:
		return "key compromise";
	case OCSP_REVOKED_STATUS_CACOMPROMISE:
		return "CA compromise";
	case OCSP_REVOKED_STATUS_AFFILIATIONCHANGED:
		return "affiliation changed";
	case OCSP_REVOKED_STATUS_SUPERSEDED:
		return "superseded";
	case OCSP_REVOKED_STATUS_CESSATIONOFOPERATION:
		return "cessation of operation";
	case OCSP_REVOKED_STATUS_CERTIFICATEHOLD:
		return "certificate hold";
	case OCSP_REVOKED_STATUS_REMOVEFROMCRL:
		return "remove from CRL";
	default:
		return NULL;
	}
}

static int print_ocsp_response_status(int status)
{
	debug_printf("*** OCSP response status: ");

	switch (status) {
	case OCSP_RESPONSE_STATUS_SUCCESSFUL:
		debug_printf("successful\n");
		break;
	case OCSP_RESPONSE_STATUS_MALFORMEDREQUEST:
		debug_printf("malformed request\n");
		break;
	case OCSP_RESPONSE_STATUS_INTERNALERROR:
		debug_printf("internal error\n");
		break;
	case OCSP_RESPONSE_STATUS_TRYLATER:
		debug_printf("try later\n");
		break;
	case OCSP_RESPONSE_STATUS_SIGREQUIRED:
		debug_printf("signature required\n");
		break;
	case OCSP_RESPONSE_STATUS_UNAUTHORIZED:
		debug_printf("unauthorized\n");
		break;
	default:
		debug_printf("unknown status code\n");
		break;
	}

	return status;
}

static int print_ocsp_cert_status(int status, int reason)
{
	const char *reason_desc;

	debug_printf("*** OCSP cert status: ");

	switch (status) {
	case V_OCSP_CERTSTATUS_GOOD:
		debug_printf("good\n");
		break;
	case V_OCSP_CERTSTATUS_UNKNOWN:
		debug_printf("unknown\n");
		break;
	default:
		debug_printf("invalid status code\n");
		break;
	case V_OCSP_CERTSTATUS_REVOKED:
		reason_desc = get_printable_ocsp_reason_desc(reason);
		debug_printf("Revoked. Reason: %s\n", (reason_desc ? reason_desc : "unknown reason"));
		break;
	}

	return status;
}

static int check_ocsp_response(OCSP_RESPONSE *ocspresp,
		STACK_OF(X509) *certstack,
		X509_STORE *certstore,
		bool check_time)
{
	int
		retval = -1,
		status, reason,
		day, sec;
	OCSP_BASICRESP *ocspbs = NULL;
	OCSP_SINGLERESP *single;
	ASN1_GENERALIZEDTIME *revtime = NULL,
			*thisupd = NULL,
			*nextupd = NULL;
	ASN1_TIME *now;

	status = OCSP_response_status(ocspresp);
	if (print_ocsp_response_status(status)
			!= OCSP_RESPONSE_STATUS_SUCCESSFUL) {
		error_printf(_("Unsuccessful OCSP response\n"));
		goto end;
	}

	if (!(ocspbs = OCSP_response_get1_basic(ocspresp)))
		goto end;

	if (!OCSP_basic_verify(ocspbs, certstack, certstore, 0)) {
		error_printf(_("Could not verify OCSP certificate chain\n"));
		goto end;
	}

	single = OCSP_resp_get0(ocspbs, 0);
	if (!single) {
		error_printf(_("Could not parse OCSP single response\n"));
		goto end;
	}

	status = OCSP_single_get0_status(single, &reason, &revtime, &thisupd, &nextupd);
	if (status == -1) {
		error_printf(_("Could not obtain OCSP response status\n"));
		goto end;
	}

	if (print_ocsp_cert_status(status, reason) != V_OCSP_CERTSTATUS_GOOD) {
		error_printf(_("Certificate revoked by OCSP\n"));
		goto end;
	}

	/* Check time is within an acceptable range */
	if (check_time) {
		if (!thisupd) {
			error_printf(_("Could not get 'thisUpd' from OCSP response. Cannot check time.\n"));
			goto end;
		}

		now = ASN1_TIME_adj(NULL, time(NULL), 0, 0);

		if (ASN1_TIME_diff(&day, &sec, now, thisupd) && day <= -3) {
			error_printf(_("OCSP response is too old. Ignoring.\n"));
			goto end;
		}
	}

	/* Success! */
	retval = 0;

end:
	if (ocspbs)
		OCSP_BASICRESP_free(ocspbs);
	return retval;
}

static int verify_ocsp(const char *ocsp_uri,
		X509 *subject_cert, X509 *issuer_cert,
		STACK_OF(X509) *certs, X509_STORE *certstore,
		bool check_time, bool check_nonce)
{
	int retval = 1;
	wget_http_response *resp;
	const unsigned char *body;
	OCSP_CERTID *certid;
	OCSP_REQUEST *ocspreq;
	OCSP_RESPONSE *ocspresp;
	OCSP_BASICRESP *ocspbs = NULL;

	/* Generate CertID and OCSP request */
	certid = OCSP_cert_to_id(EVP_sha256(), subject_cert, issuer_cert);
	if (!(ocspreq = send_ocsp_request(ocsp_uri,
			certid,
			&resp)))
		return -1;

	/* Check response */
	body = (const unsigned char *) resp->body->data;
	ocspresp = d2i_OCSP_RESPONSE(NULL, &body, resp->body->length);
	if (!ocspresp) {
		wget_http_free_response(&resp);
		OCSP_REQUEST_free(ocspreq);
		return -1;
	}

	if (check_ocsp_response(ocspresp, certs, certstore, check_time) < 0)
		goto end;

	if (check_nonce) {
		if (!(ocspbs = OCSP_response_get1_basic(ocspresp))) {
			error_printf(_("Could not obtain OCSP_BASICRESPONSE\n"));
			goto end;
		}

		if (!OCSP_check_nonce(ocspreq, ocspbs)) {
			error_printf(_("OCSP nonce does not match\n"));
			goto end;
		}

		OCSP_BASICRESP_free(ocspbs);
		ocspbs = NULL;
	}

	retval = 0; /* Success */

end:
	if (ocspbs)
		OCSP_BASICRESP_free(ocspbs);
	wget_http_free_response(&resp);
	OCSP_RESPONSE_free(ocspresp);
	OCSP_REQUEST_free(ocspreq);
	return retval;
}

static char *read_ocsp_uri_from_certificate(const X509 *cert)
{
	int idx;
	unsigned char *ocsp_uri = NULL;
	X509_EXTENSION *ext;
	ASN1_OCTET_STRING *extdata;
	const STACK_OF(X509_EXTENSION) *exts = X509_get0_extensions(cert);

	if (exts) {
		/* Read the authorityInfoAccess extension */
		if ((idx = X509v3_get_ext_by_NID(exts, NID_info_access, -1)) >= 0) {
			ext = sk_X509_EXTENSION_value(exts, idx);
			extdata = X509_EXTENSION_get_data(ext);
			if (extdata)
				ASN1_STRING_to_UTF8(&ocsp_uri, extdata);
		}
	}

	return (char *) ocsp_uri;
}

static char *compute_cert_fingerprint(X509 *cert)
{
	/*
	 * OpenSSL does not provide a function that calculates the cert fingerprint directly
	 * (like GnuTLS' gnutls_x509_crt_get_fingerprint()), but we can code it away. Fingerprint
	 * is basically a SHA-256 hash of the DER-encoded certificate.
	 */
	EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
	char *hexstring = NULL;
	unsigned char
		*der_output = NULL,
		*digest_output = NULL;
	int
		der_length,
		digest_length,
		hexstring_length;

	if ((der_length = i2d_X509(cert, &der_output)) < 0)
		goto bail;

	/* Compute SHA-256 digest of the DER-encoded certificate */
	digest_length = EVP_MD_size(EVP_sha256());
	digest_output = wget_malloc(digest_length);
	if (!digest_output)
		goto bail;

	if (!EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL))
		goto bail;
	if (!EVP_DigestUpdate(mdctx, der_output, der_length))
		goto bail;
	if (!EVP_DigestFinal_ex(mdctx, digest_output, NULL))
		goto bail;

	OPENSSL_free(der_output);
	der_output = NULL;

	EVP_MD_CTX_free(mdctx);
	mdctx = NULL;

	/* Convert SHA-256 digest to hex string */
	hexstring_length = (digest_length * 2) + 1;
	hexstring = wget_malloc(hexstring_length);
	if (!hexstring)
		goto bail;

	wget_memtohex(digest_output, digest_length, hexstring, hexstring_length);
	xfree(digest_output);
	return hexstring;

bail:
	xfree(hexstring);
	xfree(digest_output);
	if (der_output)
		OPENSSL_free(der_output);
	if (mdctx)
		EVP_MD_CTX_free(mdctx);
	return NULL;
}

static int check_cert_chain_for_ocsp(STACK_OF(X509) *certs, X509_STORE *store, const char *hostname)
{
	wget_ocsp_stats_data stats;
	int num_ok = 0, num_revoked = 0, num_ignored = 0, revoked, ocsp_ok;
	char
		*ocsp_uri = NULL,
		*fingerprint;
	X509 *cert, *issuer_cert;
	unsigned cert_list_size = sk_X509_num(certs);

	for (unsigned i = 0; i < cert_list_size; i++) {
		cert = sk_X509_value(certs, i);
		issuer_cert = sk_X509_value(certs, i+1);

		if (!issuer_cert)
			break;

		/* Compute cert fingerprint */
		fingerprint = compute_cert_fingerprint(cert);
		if (!fingerprint) {
			error_printf(_("Could not compute certificate fingerprint for cert %u\n"), i);
			return 0; /* Treat this as an error */
		}

		/* Check if there's already an OCSP response stored in cache */
		if (config.ocsp_cert_cache) {
			if (wget_ocsp_fingerprint_in_cache(config.ocsp_cert_cache, fingerprint, &revoked)) {
				/* Found cert's fingerprint in cache */
				if (revoked) {
					debug_printf("Certificate %u has been revoked (cached response)\n", i);
					num_revoked++;
				} else {
					debug_printf("Certificate %u is valid (cached response)\n", i);
					num_ok++;
				}

				xfree(fingerprint);
				continue;
			}
		}

		if (!config.ocsp_server) {
			ocsp_uri = read_ocsp_uri_from_certificate(cert);
			if (!ocsp_uri) {
				debug_printf("OCSP URI not given and not found in certificate. Skipping OCSP check for cert %u.\n",
						i);
				num_ignored++;
				xfree(fingerprint);
				continue;
			}
		}

		debug_printf("Contacting OCSP server. URI: %s\n",
				config.ocsp_server ? config.ocsp_server : ocsp_uri);

		ocsp_ok = verify_ocsp(config.ocsp_server ? config.ocsp_server : ocsp_uri,
				cert, issuer_cert, certs, store,
				config.ocsp_date, config.ocsp_nonce);
		if (ocsp_ok == 0)
			num_ok++;
		else if (ocsp_ok == 1)
			num_revoked++;

		/* Add the certificate to the OCSP cache */
		if (ocsp_ok == 0 || ocsp_ok == 1) {
			wget_ocsp_db_add_fingerprint(config.ocsp_cert_cache,
				fingerprint,
				time(NULL) + 3600, /* Cache entry valid for 1 hour */
				(ocsp_ok == 0));   /* valid? */
		}

		xfree(fingerprint);

		if (ocsp_uri)
			OPENSSL_free(ocsp_uri);
	}

	if (ocsp_stats_callback) {
		stats.hostname = hostname;
		stats.nvalid = num_ok;
		stats.nrevoked = num_revoked;
		stats.nignored = num_ignored;
		stats.stapling = 0;
		ocsp_stats_callback(&stats, ocsp_stats_ctx);
	}

	return (num_revoked == 0);
}

/*
 * This is our custom revocation check function.
 * It will be invoked by OpenSSL at some point during the TLS handshake.
 * It takes the server's certificate chain, and its purpose is to check the revocation
 * status for each certificate in it. We validate certs against HPKP and OCSP here.
 * OpenSSL will make other checks before calling this function: cert signature, CRLs, etc.
 * This function should return the value of 'ossl_retval' on success
 * (which retains the result of previous checks made by OpenSSL) and 0 on failure (will override
 * OpenSSL's result, whatever it is).
 */
static int openssl_revocation_check_fn(int ossl_retval, X509_STORE_CTX *storectx)
{
	X509_STORE *store;
	struct verification_flags *vflags;
	STACK_OF(X509) *certs = X509_STORE_CTX_get1_chain(storectx);

	if (ossl_retval == 0) {
		/* ossl_retval == 0 means certificate was revoked by OpenSSL before entering this callback */
		goto end;
	}

	store = X509_STORE_CTX_get0_store(storectx);
	if (!store) {
		error_printf(_("Could not retrieve certificate store. Will skip HPKP checks.\n"));
		goto end;
	}

	vflags = X509_STORE_get_ex_data(store, store_userdata_idx);
	if (!vflags) {
		error_printf(_("Could not retrieve saved verification status flags.\n"));
		goto end;
	}

	if (vflags->verifying_ocsp)
		goto end;

	/* Store the certificate chain size */
	vflags->cert_chain_size = sk_X509_num(certs);

	if (config.hpkp_cache) {
		/* Check cert chain against HPKP database */
		if (!check_cert_chain_for_hpkp(certs, vflags->hostname, &vflags->hpkp_stats)) {
			error_printf(_("Public key pinning mismatch.\n"));
			ossl_retval = 0;
			goto end;
		}
	}

	if (config.ocsp && !vflags->ocsp_checked) {
		/* Check cert chain against OCSP */
		vflags->verifying_ocsp = 1;

		if (!check_cert_chain_for_ocsp(certs, store, vflags->hostname)) {
			error_printf(_("Certificate revoked by OCSP.\n"));
			ossl_retval = 0;
			goto end;
		}

		vflags->ocsp_checked = 1;
		vflags->verifying_ocsp = 0;
	}

end:
	sk_X509_pop_free(certs, X509_free);
	return ossl_retval;
}

static int openssl_init(SSL_CTX *ctx)
{
	int retval = 0;
	X509_STORE *store;

	if (!config.check_certificate) {
		SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
		info_printf(_("Certificate check disabled. Peer's certificate will NOT be checked.\n"));
		goto end;
	}

	store = SSL_CTX_get_cert_store(ctx);
	if (!store) {
		error_printf(_("OpenSSL: Could not obtain cert store\n"));
		retval = WGET_E_UNKNOWN;
		goto end;
	}

	if (config.ca_directory && *config.ca_directory) {
		retval = openssl_load_trust_files(ctx, config.ca_directory);
		if (retval < 0)
			goto end;

		if (config.crl_file) {
			/* Load CRL file in PEM format. */
			if ((retval = openssl_load_crl(store, config.crl_file)) < 0) {
				error_printf(_("Could not load CRL from '%s' (%d)\n"),
					config.crl_file,
					retval);
				goto end;
			}
		}

		SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
	}

	/* Load individual CA file, if requested */
	if (config.ca_file && *config.ca_file
		&& !SSL_CTX_load_verify_locations(ctx, config.ca_file, NULL))
	{
		error_printf(_("Could not load CA certificate from file '%s'\n"), config.ca_file);
	}

#ifdef WITH_OCSP
	if (config.ocsp_stapling)
		SSL_CTX_set_tlsext_status_cb(ctx, ocsp_resp_cb);
#endif

	/* Set our custom revocation check function, for HPKP and OCSP validation */
	X509_STORE_set_verify_cb(store, openssl_revocation_check_fn);

	retval = openssl_set_priorities(ctx, config.secure_protocol);

end:
	return retval;
}

static void openssl_deinit(SSL_CTX *ctx)
{
	SSL_CTX_free(ctx);
}

/**
 * Initialize the SSL/TLS engine as a client.
 *
 * This function assumes the caller is an SSL client connecting to a server.
 * The functions wget_ssl_open(), wget_ssl_close() and wget_ssl_deinit() can be called
 * after this.
 *
 * This is where the root certificates get loaded from the folder specified in the
 * `WGET_SSL_CA_DIRECTORY` parameter. If any of the files in that folder cannot be loaded
 * for whatever reason, that file will be silently skipped without harm (a message will be
 * printed to the debug log stream).
 *
 * CLRs and private keys and their certificates are also loaded here.
 *
 * On systems with automatic library constructors/destructors, this function
 * is thread-safe. On other systems it is not thread-safe.
 *
 * This function may be called several times. Only the first call really
 * takes action.
 */
void wget_ssl_init(void)
{
	wget_thread_mutex_lock(mutex);

	if (!init) {
		_ctx = SSL_CTX_new(TLS_client_method());
		if (_ctx && openssl_init(_ctx) == 0) {
			init++;
#ifdef LIBRESSL_VERSION_NUMBER
			debug_printf("LibreSSL initialized\n");
#else
			debug_printf("OpenSSL initialized\n");
#endif
		} else {
			error_printf(_("Could not initialize OpenSSL\n"));
		}
	}

	wget_thread_mutex_unlock(mutex);
}

/**
 * Deinitialize the SSL/TLS engine, after it has been initialized
 * with wget_ssl_init().
 *
 * This function unloads everything that was loaded in wget_ssl_init().
 *
 * On systems with automatic library constructors/destructors, this function
 * is thread-safe. On other systems it is not thread-safe.
 *
 * This function may be called several times. Only the last deinit really
 * takes action.
 */
void wget_ssl_deinit(void)
{
	wget_thread_mutex_lock(mutex);

	if (init == 1)
		openssl_deinit(_ctx);

	if (init > 0)
		init--;

	wget_thread_mutex_unlock(mutex);
}

static int ssl_resume_session(SSL *ssl, const char *hostname)
{
	void *sess = NULL;
	size_t sesslen;
	SSL_SESSION *ssl_session;

	if (!config.tls_session_cache)
		return 0;

	if (wget_tls_session_get(config.tls_session_cache,
			hostname,
			&sess, &sesslen) == 0
		&& sess)
	{
		debug_printf("Found cached session data for host '%s'\n",hostname);
		ssl_session = d2i_SSL_SESSION(NULL,
				(const unsigned char **) &sess,
				(long) sesslen);
		if (!ssl_session) {
			error_printf(_("OpenSSL: Could not parse cached session data.\n"));
			return -1;
		}
#if OPENSSL_VERSION_NUMBER >= 0x10101000 && !defined LIBRESSL_VERSION_NUMBER
		if (!SSL_SESSION_is_resumable(ssl_session))
			return -1;
#endif
		if (!SSL_set_session(ssl, ssl_session)) {
			error_printf(_("OpenSSL: Could not set session data.\n"));
			return -1;
		}

		SSL_SESSION_free(ssl_session);
		return 1;
	}

	return 0;
}

static int ssl_save_session(const SSL *ssl, const char *hostname)
{
	void *sess = NULL;
	unsigned long sesslen;
	SSL_SESSION *ssl_session = SSL_get0_session(ssl);

	if (!ssl_session || !config.tls_session_cache)
		return 0;

	sesslen = i2d_SSL_SESSION(ssl_session, (unsigned char **) &sess);
	if (sesslen) {
		wget_tls_session_db_add(config.tls_session_cache,
			wget_tls_session_new(hostname,
				18 * 3600, /* session valid for 18 hours */
				sess, sesslen));
		OPENSSL_free(sess);
		return 1;
	}

	return 0;
}

static int wait_2_read_and_write(int sockfd, int timeout)
{
	int retval = wget_ready_2_transfer(sockfd,
			timeout,
			WGET_IO_READABLE | WGET_IO_WRITABLE);

	if (retval == 0)
		retval = WGET_E_TIMEOUT;

	return retval;
}

static bool ssl_set_alpn_offering(SSL *ssl, const char *alpn)
{
	int ret = WGET_E_UNKNOWN;
	const char *s, *e;
	char sbuf[32];
	wget_buffer buf;

	wget_buffer_init(&buf, sbuf, sizeof(sbuf));

	for (s = e = alpn; *e; s = e + 1) {
		if ((e = strchrnul(s, ',')) != s) {
			if (e - s > 64) { // let's be reasonable
				debug_printf("ALPN protocol too long %.*s\n", (int) (e - s), s);
				continue;
			}

			debug_printf("ALPN offering %.*s\n", (int) (e - s), s);
			wget_buffer_memset_append(&buf, (e - s) & 0x7F, 1); // length of protocol string
			wget_buffer_memcat(&buf, s, e - s);
		}
	}

	if (buf.length) {
		if (SSL_set_alpn_protos(ssl, (unsigned char *) buf.data, (unsigned) buf.length)) {
			debug_printf("OpenSSL: ALPN: Could not set ALPN offering");
		} else
			ret = WGET_E_SUCCESS;
	}

	wget_buffer_deinit(&buf);

	return ret;
}

static void ssl_set_alpn_selected_protocol(const SSL *ssl, wget_tcp *tcp, wget_tls_stats_data *stats)
{
	const unsigned char *data;
	unsigned int datalen;

	SSL_get0_alpn_selected(ssl, &data, &datalen);

	if (data && datalen) {
		debug_printf("ALPN: Server accepted protocol '%.*s'\n", (int) datalen, data);

		/* Success - Set selected protocol and update stats */
		if (stats)
			stats->alpn_protocol = wget_strmemdup(data, datalen);

		if (datalen == 2 && data[0] == 'h' && data[1] == '2') {
			tcp->protocol = WGET_PROTOCOL_HTTP_2_0;
			if (stats)
				stats->http_protocol = WGET_PROTOCOL_HTTP_2_0;
		}
	}
}

static int get_tls_version(const SSL *ssl)
{
	int version = SSL_version(ssl);

	/*
	 * These values are mapped to the return values of GnuTLS' function
	 * gnutls_protocol_get_version() - integers on a gnutls_protocol_t enum.
	 *
	 * See: https://gitlab.com/gnutls/gnutls/blob/master/lib/includes/gnutls/gnutls.h.in#L736
	 */
	switch (version) {
	case SSL3_VERSION:
		/* SSL v3 */
		return 1;
	case TLS1_VERSION:
		/* TLS 1.0 */
		return 2;
	case TLS1_1_VERSION:
		/* TLS 1.1 */
		return 3;
	case TLS1_2_VERSION:
		/* TLS 1.2 */
		return 4;
#if TLS1_2_VERSION != TLS1_3_VERSION
	case TLS1_3_VERSION:
		/* TLS 1.3 */
		return 5;
#endif
	default:
		return -1;
	}
}

/**
 * \param[in] tcp A TCP connection (see wget_tcp_init())
 * \return `WGET_E_SUCCESS` on success or an error code (`WGET_E_*`) on failure
 *
 * Run an SSL/TLS handshake.
 *
 * This functions establishes an SSL/TLS tunnel (performs an SSL/TLS handshake)
 * over an active TCP connection. A pointer to the (internal) SSL/TLS session context
 * can be found in `tcp->ssl_session` after successful execution of this function. This pointer
 * has to be passed to wget_ssl_close() to close the SSL/TLS tunnel.
 *
 * If the handshake cannot be completed in the specified timeout for the provided TCP connection
 * this function fails and returns `WGET_E_TIMEOUT`. You can set the timeout with wget_tcp_set_timeout().
 */
int wget_ssl_open(wget_tcp *tcp)
{
	SSL *ssl = NULL;
	X509_STORE *store;
	int retval, error, resumed;
	struct verification_flags *vflags = NULL;
	wget_tls_stats_data stats = {
		.alpn_protocol = NULL,
		.version = -1,
		.false_start = 0,
		.tfo = 0,
		.resumed = 0,
		.http_protocol = WGET_PROTOCOL_HTTP_1_1,
		.cert_chain_size = 0
	}, *stats_p = NULL;

	if (!tcp || tcp->sockfd < 0)
		return WGET_E_INVALID;
	if (!init)
		wget_ssl_init();

	/* Initiate a new TLS connection from an existing OpenSSL context */
	if (!(ssl = SSL_new(_ctx)) || !SSL_set_fd(ssl, FD_TO_SOCKET(tcp->sockfd))) {
		retval = WGET_E_UNKNOWN;
		goto bail;
	}

	/* Store state flags for the verification callback */
	vflags = wget_malloc(sizeof(struct verification_flags));
	if (!vflags) {
		retval = WGET_E_MEMORY;
		goto bail;
	}

	vflags->ocsp_checked = 0;
	vflags->verifying_ocsp = 0;
	vflags->cert_chain_size = 0;
	vflags->hostname = tcp->ssl_hostname;

	if (store_userdata_idx == -1) {
		retval = WGET_E_UNKNOWN;
		goto bail;
	}

	store = SSL_CTX_get_cert_store(_ctx);
	if (!store) {
		retval = WGET_E_UNKNOWN;
		goto bail;
	}

	vflags->certstore = store;
	vflags->ssl = ssl;

	if (!X509_STORE_set_ex_data(store, store_userdata_idx, (void *) vflags)) {
		retval = WGET_E_UNKNOWN;
		goto bail;
	}
#ifdef WITH_OCSP
	SSL_CTX_set_tlsext_status_arg(_ctx, vflags);
#endif


	/* Enable stats logging, if requested */
	if (tls_stats_callback)
		stats_p = &stats;

	/* Enable host name verification, if requested */
	if (config.check_hostname) {
		SSL_set1_host(ssl, tcp->ssl_hostname);
#ifndef LIBRESSL_VERSION_NUMBER
// LibreSSL <= 3.0.2 does not know SSL_set_hostflags()
		SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
#endif
	}
#ifndef LIBRESSL_VERSION_NUMBER
// LibreSSL <= 3.0.2 does not know SSL_set_hostflags() nor X509_CHECK_FLAG_NEVER_CHECK_SUBJECT
	else {
		SSL_set_hostflags(ssl, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);
		info_printf(_("Host name check disabled. Server certificate's subject name will not be checked.\n"));
	}
#endif

#ifdef WITH_OCSP
	if (config.ocsp_stapling) {
		if (SSL_set_tlsext_status_type(ssl, TLSEXT_STATUSTYPE_ocsp))
			debug_printf("Sending 'status_request' extension in handshake\n");
		else
			error_printf(_("Could not set 'status_request' extension\n"));
	}
#endif

	/* Send Server Name Indication (SNI) */
	if (tcp->ssl_hostname && !SSL_set_tlsext_host_name(ssl, tcp->ssl_hostname))
		error_printf(_("SNI could not be sent"));

	/* Send ALPN if requested */
	if (config.alpn && ssl_set_alpn_offering(ssl, config.alpn))
		error_printf(_("ALPN offering could not be sent"));

	/* Resume from a previous SSL/TLS session, if available */
	if ((resumed = ssl_resume_session(ssl, tcp->ssl_hostname)) == 1)
		debug_printf("Will try to resume cached TLS session");
	else if (resumed == 0)
		debug_printf("No cached TLS session available. Will run a full handshake.");
	else
		error_printf(_("Could not get cached TLS session"));

	do {
		/* Wait for socket to become ready */
		if (tcp->connect_timeout &&
			(retval = wait_2_read_and_write(tcp->sockfd, tcp->connect_timeout)) < 0)
			goto bail;

		/* Run TLS handshake */
		retval = SSL_connect(ssl);
		if (retval > 0) {
			error = 0;
			resumed = SSL_session_reused(ssl);
			break;
		}

		error = SSL_get_error(ssl, retval);
	} while (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);

	if (retval <= 0) {
		/* Error! Tell the user what happened, and exit. */
		if (error == SSL_ERROR_SSL) {
			error_printf(_("Could not complete TLS handshake: %s\n"),
				ERR_reason_error_string(ERR_peek_last_error()));
		}

		/* Return proper error code - Most of the time this will be a cert validation error */
		retval = (ERR_GET_REASON(ERR_peek_last_error()) == SSL_R_CERTIFICATE_VERIFY_FAILED ?
			WGET_E_CERTIFICATE :
			WGET_E_HANDSHAKE);
		goto bail;
	}

	/* Success! */
	debug_printf("Handshake completed%s\n", resumed ? " (resumed session)" : " (full handshake - not resumed)");

	/* Save the current TLS session */
	if (ssl_save_session(ssl, tcp->ssl_hostname))
		debug_printf("TLS session saved in cache");
	else
		debug_printf("TLS session discarded");

	/* Set the protocol selected by the server via ALPN, if any */
	if (config.alpn)
		ssl_set_alpn_selected_protocol(ssl, tcp, stats_p);

	if (stats_p) {
		stats_p->version = get_tls_version(ssl);
		stats_p->hostname = vflags->hostname;
		stats_p->resumed = resumed;
		stats_p->cert_chain_size = vflags->cert_chain_size;
		tls_stats_callback(stats_p, tls_stats_ctx);
		xfree(stats_p->alpn_protocol);

#ifdef MSG_FASTOPEN
		stats_p->tfo = wget_tcp_get_tcp_fastopen(tcp);
#endif
	}

	tcp->hpkp = vflags->hpkp_stats;
	tcp->ssl_session = ssl;
	xfree(vflags);
	return WGET_E_SUCCESS;

bail:
	if (stats_p)
		xfree(stats_p->alpn_protocol);
	xfree(vflags);
	if (ssl)
		SSL_free(ssl);
	return retval;
}

/**
 * \param[in] session The SSL/TLS session (a pointer to it), which is located at the `ssl_session` field
 * of the TCP connection (see wget_ssl_open()).
 *
 * Close an active SSL/TLS tunnel, which was opened with wget_ssl_open().
 *
 * The underlying TCP connection is kept open.
 */
void wget_ssl_close(void **session)
{
	SSL *ssl;
	int retval;

	if (session && *session) {
		ssl = *session;

		do
			retval = SSL_shutdown(ssl);
		while (retval == 0);

		SSL_free(ssl);
		*session = NULL;
	}
}

static int ssl_transfer(int want,
		void *session, int timeout,
		void *buf, int count)
{
	SSL *ssl;
	int fd, retval, error, ops = want;

	if (count == 0)
		return 0;
	if ((ssl = session) == NULL)
		return WGET_E_INVALID;
	if ((fd = SOCKET_TO_FD(SSL_get_fd(ssl))) < 0)
		return WGET_E_UNKNOWN;

	if (timeout < -1)
		timeout = -1;

	do {
		if (timeout) {
			/* Wait until file descriptor becomes ready */
			retval = wget_ready_2_transfer(fd, timeout, ops);
			if (retval < 0)
				return retval;
			else if (retval == 0)
				return WGET_E_TIMEOUT;
		}

		/* We assume socket is non-blocking so neither of these should block */
		if (want == WGET_IO_READABLE)
			retval = SSL_read(ssl, buf, count);
		else
			retval = SSL_write(ssl, buf, count);

		if (retval < 0) {
			error = SSL_get_error(ssl, retval);

			if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
				/* Socket not ready - let's try again (unless timeout was zero) */
				ops = WGET_IO_WRITABLE | WGET_IO_READABLE;

				if (timeout == 0)
					return 0;
			} else {
				/* Not exactly a handshake error, but this is the closest one to signal TLS layer errors */
				return WGET_E_HANDSHAKE;
			}
		}
	} while (retval < 0);

	return retval;
}

/**
 * \param[in] session An opaque pointer to the SSL/TLS session (obtained with wget_ssl_open() or wget_ssl_server_open())
 * \param[in] buf Destination buffer where the read data will be placed
 * \param[in] count Length of the buffer \p buf
 * \param[in] timeout The amount of time to wait until data becomes available (in milliseconds)
 * \return The number of bytes read, or a negative value on error.
 *
 * Read data from the SSL/TLS tunnel.
 *
 * This function will read at most \p count bytes, which will be stored
 * in the buffer \p buf.
 *
 * The \p timeout parameter tells how long to wait until some data becomes
 * available to read. A \p timeout value of zero causes this function to return
 * immediately, whereas a negative value will cause it to wait indefinitely.
 * This function returns the number of bytes read, which may be zero if the timeout elapses
 * without any data having become available.
 *
 * If a rehandshake is needed, this function does it automatically and tries
 * to read again.
 */
ssize_t wget_ssl_read_timeout(void *session,
	char *buf, size_t count,
	int timeout)
{
	int retval = ssl_transfer(WGET_IO_READABLE, session, timeout, buf, (int) count);

	if (retval == WGET_E_HANDSHAKE) {
		error_printf(_("TLS read error: %s\n"),
			ERR_reason_error_string(ERR_peek_last_error()));
		retval = WGET_E_UNKNOWN;
	}

	return retval;
}

/**
 * \param[in] session An opaque pointer to the SSL/TLS session (obtained with wget_ssl_open() or wget_ssl_server_open())
 * \param[in] buf Buffer with the data to be sent
 * \param[in] count Length of the buffer \p buf
 * \param[in] timeout The amount of time to wait until data can be sent to the wire (in milliseconds)
 * \return The number of bytes written, or a negative value on error.
 *
 * Send data through the SSL/TLS tunnel.
 *
 * This function will write \p count bytes from \p buf.
 *
 * The \p timeout parameter tells how long to wait until data can be finally sent
 * over the SSL/TLS tunnel. A \p timeout value of zero causes this function to return
 * immediately, whereas a negative value will cause it to wait indefinitely.
 * This function returns the number of bytes sent, which may be zero if the timeout elapses
 * before any data could be sent.
 *
 * If a rehandshake is needed, this function does it automatically and tries
 * to write again.
 */
ssize_t wget_ssl_write_timeout(void *session,
	const char *buf, size_t count,
	int timeout)
{
	int retval = ssl_transfer(WGET_IO_WRITABLE, session, timeout, (void *) buf, (int) count);

	if (retval == WGET_E_HANDSHAKE) {
		error_printf(_("TLS write error: %s\n"),
			ERR_reason_error_string(ERR_peek_last_error()));
		retval = WGET_E_UNKNOWN;
	}

	return retval;
}

/**
 * \param[in] fn A `wget_ssl_stats_callback_tls` callback function to receive TLS statistics data
 * \param[in] ctx Context data given to \p fn
 *
 * Set callback function to be called when TLS statistics are available
 */
void wget_ssl_set_stats_callback_tls(wget_tls_stats_callback fn, void *ctx)
{
	tls_stats_callback = fn;
	tls_stats_ctx = ctx;
}

/**
 * \param[in] fn A `wget_ssl_stats_callback_ocsp` callback function to receive OCSP statistics data
 * \param[in] ctx Context data given to \p fn
 *
 * Set callback function to be called when OCSP statistics are available
 */
void wget_ssl_set_stats_callback_ocsp(wget_ocsp_stats_callback fn, void *ctx)
{
	ocsp_stats_callback = fn;
	ocsp_stats_ctx = ctx;
}

/** @} */
