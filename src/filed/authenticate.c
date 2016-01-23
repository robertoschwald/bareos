/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2010 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2016 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * Authenticate Director who is attempting to connect.
 *
 * Kern Sibbald, October 2000
 */

#include "bareos.h"
#include "filed.h"

const int dbglvl = 50;

/* Version at end of Hello
 *   prior to 10Mar08 no version
 *   1 10Mar08
 *   2 13Mar09 - Added the ability to restore from multiple storages
 *   3 03Sep10 - Added the restore object command for vss plugin 4.0
 *   4 25Nov10 - Added bandwidth command 5.1
 *   5 24Nov11 - Added new restore object command format (pluginname) 6.0
 *
 *  51 21Mar13 - Added reverse datachannel initialization
 *  52 13Jul13 - Added plugin options
 *  53 02Apr15 - Added setdebug timestamp
 *  54 29Oct15 - Added getSecureEraseCmd
 */
static char OK_hello_compat[] =
   "2000 OK Hello 5\n";
static char OK_hello[] =
   "2000 OK Hello 54\n";

static char Dir_sorry[] =
   "2999 Authentication failed.\n";

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Depending on the initiate parameter perform one of the following:
 *
 * - First make him prove his identity and then prove our identity to the Remote.
 * - First prove our identity to the Remote and then make him prove his identity.
 */
static inline bool two_way_authenticate(BSOCK *bs, JCR *jcr, const char *what,
                                        const char *name, s_password &password,
                                        tls_t &tls, bool initiated_by_remote)
{
   int tls_local_need = BNET_TLS_NONE;
   int tls_remote_need = BNET_TLS_NONE;
   bool compatible = true;
   bool auth_success = false;
   btimer_t *tid = NULL;

   ASSERT(password.encoding == p_encoding_md5);

   /*
    * TLS Requirement
    */
   if (have_tls && tls.enable) {
      if (tls.require) {
         tls_local_need = BNET_TLS_REQUIRED;
      } else {
         tls_local_need = BNET_TLS_OK;
      }
   }

   if (tls.authenticate) {
      tls_local_need = BNET_TLS_REQUIRED;
   }

   if (job_canceled(jcr)) {
      auth_success = false;     /* force quick exit */
      goto auth_fatal;
   }

   /*
    * Timeout Hello after 10 min
    */
   tid = start_bsock_timer(bs, AUTH_TIMEOUT);

   /*
    * See if we initiate the challenge or respond to a challenge.
    */
   if (initiated_by_remote) {
      /*
       * Challenge SD
       */
      auth_success = cram_md5_challenge(bs, password.value, tls_local_need, compatible);
      if (auth_success) {
          /*
           * Respond to his challenge
           */
          auth_success = cram_md5_respond(bs, password.value, &tls_remote_need, &compatible);
          if (!auth_success) {
             Dmsg1(dbglvl, "Respond cram-get-auth failed with %s\n", bs->who());
          }
      } else {
         Dmsg1(dbglvl, "Challenge cram-auth failed with %s\n", bs->who());
      }
   } else {
      /*
       * Respond to challenge
       */
      auth_success = cram_md5_respond(bs, password.value, &tls_remote_need, &compatible);
      if (job_canceled(jcr)) {
         auth_success = false;     /* force quick exit */
         goto auth_fatal;
      }
      if (!auth_success) {
         Dmsg1(dbglvl, "cram_respond failed for %s\n", bs->who());
      } else {
         /*
          * Challenge.
          */
         auth_success = cram_md5_challenge(bs, password.value, tls_local_need, compatible);
         if (!auth_success) {
            Dmsg1(dbglvl, "cram_challenge failed for %s\n", bs->who());
         }
      }
   }

   if (!auth_success) {
      Jmsg(jcr, M_FATAL, 0, _("Authorization key rejected by %s %s.\n"
                              "Please see %s for help.\n"),
                              what, name, MANUAL_AUTH_URL);
      goto auth_fatal;
   }

   /*
    * Verify that the remote host is willing to meet our TLS requirements
    */
   if (tls_remote_need < tls_local_need && tls_local_need != BNET_TLS_OK && tls_remote_need != BNET_TLS_OK) {
      Jmsg(jcr, M_FATAL, 0, _("Authorization problem: Remote server did not"
                              " advertize required TLS support.\n"));
      Dmsg2(dbglvl, "remote_need=%d local_need=%d\n", tls_remote_need, tls_local_need);
      auth_success = false;
      goto auth_fatal;
   }

   /*
    * Verify that we are willing to meet the remote host's requirements
    */
   if (tls_remote_need > tls_local_need && tls_local_need != BNET_TLS_OK && tls_remote_need != BNET_TLS_OK) {
      Jmsg(jcr, M_FATAL, 0, _("Authorization problem: Remote server requires TLS.\n"));
      Dmsg2(dbglvl, "remote_need=%d local_need=%d\n", tls_remote_need, tls_local_need);
      auth_success = false;
      goto auth_fatal;
   }

   if (tls_local_need >= BNET_TLS_OK && tls_remote_need >= BNET_TLS_OK) {
      alist *verify_list = NULL;

      if (tls.verify_peer) {
         verify_list = tls.allowed_cns;
      }

      /*
       * See if we are handshaking a passive client connection.
       */
      if (initiated_by_remote) {
         if (!bnet_tls_server(tls.ctx, bs, verify_list)) {
            Jmsg(jcr, M_FATAL, 0, _("TLS negotiation failed.\n"));
            Dmsg0(dbglvl, "TLS negotiation failed.\n");
            auth_success = false;
            goto auth_fatal;
         }
      } else {
         if (!bnet_tls_client(tls.ctx, bs, tls.verify_peer, verify_list)) {
            Jmsg(jcr, M_FATAL, 0, _("TLS negotiation failed.\n"));
            Dmsg0(dbglvl, "TLS negotiation failed.\n");
            auth_success = false;
            goto auth_fatal;
         }
      }

      if (tls.authenticate) {           /* tls authentication only? */
         bs->free_tls();                    /* yes, shutdown tls */
      }
   }

auth_fatal:
   if (tid) {
      stop_bsock_timer(tid);
      tid = NULL;
   }

   jcr->authenticated = auth_success;

   /*
    * Single thread all failures to avoid DOS
    */
   if (!auth_success) {
      P(mutex);
      bmicrosleep(6, 0);
      V(mutex);
   }

   return auth_success;
}

/*
 * Original version of this function, used to authenticate from fd to sd.
 */
static inline bool two_way_authenticate(BSOCK *bs, JCR *jcr, bool initiated_by_remote, const char *what)
{
    bool result;
    s_password password;
    const char name[] = "";

    password.encoding = p_encoding_md5;
    password.value = jcr->sd_auth_key;

    result = two_way_authenticate(bs, jcr, what, name, password, me->tls, initiated_by_remote);

    /*
     * Destroy session key
     */
    memset(jcr->sd_auth_key, 0, strlen(jcr->sd_auth_key));

    return result;
}

/*
 * Inititiate the communications with the Director.
 * He has made a connection to our server.
 *
 * Basic tasks done here:
 * We read Director's initial message and authorize him.
 */
bool authenticate_director(JCR *jcr)
{
   const bool initiated_by_remote = true;
   BSOCK *dir = jcr->dir_bsock;

   POOL_MEM dirname(PM_MESSAGE);
   DIRRES *director = NULL;

   if (dir->msglen < 25 || dir->msglen > 500) {
      Dmsg2(dbglvl, "Bad Hello command from Director at %s. Len=%d.\n",
            dir->who(), dir->msglen);
      char addr[64];
      char *who = bnet_get_peer(dir, addr, sizeof(addr)) ? dir->who() : addr;
      Jmsg2(jcr, M_FATAL, 0, _("Bad Hello command from Director at %s. Len=%d.\n"),
             who, dir->msglen);
      return false;
   }

   if (sscanf(dir->msg, "Hello Director %s calling", dirname.check_size(dir->msglen)) != 1) {
      char addr[64];
      char *who = bnet_get_peer(dir, addr, sizeof(addr)) ? dir->who() : addr;

      dir->msg[100] = 0;
      Dmsg2(dbglvl, "Bad Hello command from Director at %s: %s\n", dir->who(), dir->msg);
      Jmsg2(jcr, M_FATAL, 0, _("Bad Hello command from Director at %s: %s\n"), who, dir->msg);
      return false;
   }

   unbash_spaces(dirname.c_str());
   director = (DIRRES *)GetResWithName(R_DIRECTOR, dirname.c_str());

   if (!director) {
      char addr[64];
      char *who = bnet_get_peer(dir, addr, sizeof(addr)) ? dir->who() : addr;
      Jmsg2(jcr, M_FATAL, 0, _("Connection from unknown Director %s at %s rejected.\n"), dirname.c_str(), who);
      return false;
   }

   if (!director->connection_from_director_to_client) {
      Jmsg1(jcr, M_FATAL, 0, _("Connection from Director %s is rejected.\n"), dirname.c_str());
      return false;
   }

   if (!two_way_authenticate(dir, jcr, "Director",
                             dirname.c_str(), director->password, director->tls, initiated_by_remote)) {
      dir->fsend("%s", Dir_sorry);
      Emsg0(M_FATAL, 0, _("Unable to authenticate Director\n"));
      return false;
   }

   jcr->director = director;

   return dir->fsend("%s", (me->compatible) ? OK_hello_compat : OK_hello);
}

/*
 * Authenticate with a remote director.
 */
bool authenticate_with_director(JCR *jcr, DIRRES *dir_res)
{
   const bool initiated_by_remote = false;
   BSOCK *dir = jcr->dir_bsock;

   return two_way_authenticate(dir, jcr, "Director",
                               dir_res->name(), dir_res->password,
                               dir_res->tls, initiated_by_remote);
}

/*
 * Authenticate a remote storage daemon.
 */
bool authenticate_storagedaemon(JCR *jcr)
{
   const bool initiated_by_remote = true;
   BSOCK *sd = jcr->store_bsock;

   return two_way_authenticate(sd, jcr, initiated_by_remote, "Storage daemon");
}

/*
 * Authenticate with a remote storage daemon.
 */
bool authenticate_with_storagedaemon(JCR *jcr)
{
   const bool initiated_by_remote = false;
   BSOCK *sd = jcr->store_bsock;

   return two_way_authenticate(sd, jcr, initiated_by_remote, "Storage daemon");
}
