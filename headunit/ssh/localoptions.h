/* Dropbear's build options for the head unit (see README): one connection per process (-i), public keys
 * only, nothing written outside paths given on the command line. Compression is built out by configure's
 * --disable-zlib, and the client (dbclient) is not built. */

/* No default host key paths under /etc: -r is always given. */
#define DSS_PRIV_FILENAME "/nonexistent/dss"
#define RSA_PRIV_FILENAME "/nonexistent/rsa"
#define ECDSA_PRIV_FILENAME "/nonexistent/ecdsa"
#define ED25519_PRIV_FILENAME "/nonexistent/ed25519"
#define DROPBEAR_PIDFILE "/nonexistent/dropbear.pid"

#define NON_INETD_MODE 1
#define INETD_MODE 1
#define DROPBEAR_REEXEC 0

#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_SVR_PUBKEY_OPTIONS 1
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_X11FWD 0
#define DROPBEAR_USE_PASSWORD_ENV 0

#define DO_MOTD 0

/* QNX 6.5 has no setresgid(), which privilege dropping needs. Root logs in
 * as root, so nothing is dropped; Unix-socket forwarding needs dropping. */
#define DROPBEAR_SVR_DROP_PRIVS 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0

/* The head unit's own shell PATH; kept only when -e is not given. */
#define DEFAULT_ROOT_PATH "/proc/boot:/boot/bin:/bin:/opt/os/bin:/opt/sys/bin:/opt/core/bin:/opt/car/bin"
#define DEFAULT_PATH DEFAULT_ROOT_PATH

/* Looked up in PATH by the user's shell. */
#define SFTPSERVER_PATH "sftp-server"

/* DROPBEAR_PATH_SSH_PROGRAM is left as it is: with no dbclient, scp here is only the far end of a
 * client's copy (scp -t, scp -f), which starts no SSH program. */
