#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <apr.h>
#include <apr_strings.h>
#include <apr_hash.h>
#include <apr_time.h>
#include <apr_pools.h>
#include <apr_poll.h>
#include <apr_network_io.h>
#include <apr_signal.h>       
#include <apr_thread_proc.h>  /* for apr_proc_detach(). no threads used. */
#include <apr_tables.h>

#include "cmdline.h"   /* generated by cmdline.sh which runs gengetopt */
#include "confuse.h"   /* header for libconfuse in ./srclib/confuse */
#include "conf.h"      /* configuration file definition in libconfuse format */
#include "become_a_nobody.h"
#include "libmetrics.h"/* libmetrics header in ./srclib/libmetrics */
#include "apr_net.h"   /* our private network functions based on apr */
#include "debug_msg.h" 
#include "protocol.h"  /* generated header from ./lib/protocol.x xdr definition file */
#include "dtd.h"       /* the DTD definition for our XML */

/* When this gmond was started */
apr_time_t started;
/* My name */
char myname[APRMAXHOSTLEN+1];
/* The commandline options */
struct gengetopt_args_info args_info;
/* The configuration file */
cfg_t *config_file;
/* The debug level (in debug_msg.c) */
extern int debug_level;
/* The global context */
apr_pool_t *global_context = NULL;
/* Deaf mode boolean */
int deaf;
/* Mute mode boolean */
int mute;
/* Maximum UDP message size.. TODO: allow tweakability */
int max_udp_message_len = 1472; /* mtu 1500 - 28 bytes for IP/UDP headers */
/* The pollset for incoming UDP messages */
apr_pollset_t *udp_recv_pollset = NULL;
/* The access control list for each of the UDP channels */
apr_array_header_t *udp_recv_acl_array = NULL;

/* The array for outgoing UDP message channels */
apr_array_header_t *udp_send_array = NULL;

/* The pollset for incoming TCP requests */
apr_pollset_t *tcp_accept_pollset = NULL;
/* The access control list for each of the TCP accept channels */
apr_array_header_t *tcp_accept_acl_array = NULL;

/* The hash to hold the hosts (key = host IP) */
apr_hash_t *hosts = NULL;

/* The "hosts" hash contains values of type "hostdata" */
struct Ganglia_host_data {
  /* Name of the host */
  char *hostname;
  /* The IP of this host */
  char *ip;
  /* Timestamp of when the remote host gmond started */
  unsigned int gmond_started;
  /* The pool used to malloc memory for this host */
  apr_pool_t *pool;
  /* A hash containing the registered data from the host */
  apr_hash_t *metrics;
  /* A hash containing the arbitrary data from gmetric */
  apr_hash_t *gmetrics;
  /* First heard from */
  apr_time_t first_heard_from;
  /* Last heard from */
  apr_time_t last_heard_from;
};
typedef struct Ganglia_host_data Ganglia_host_data;

struct Ganglia_metric_data {
  /* The ganglia message */
  Ganglia_message message;
  /* Last heard from */
  apr_time_t last_heard_from;
};
typedef struct Ganglia_metric_data Ganglia_metric_data;

/* The hash to hold the metrics available on this platform */
apr_hash_t *metric_callbacks = NULL;

/* The "metrics" hash contains values of type "Ganglia_metric_callback" */
/* This is where libmetrics meets gmond */
struct Ganglia_metric_callback {
   char *name; /* metric name */
   g_val_t (*cb)(void); /* callback function */
};
typedef struct Ganglia_metric_callback Ganglia_metric_callback;

static void
cleanup_configuration_file(void)
{
  cfg_free( config_file );
}

static void
process_configuration_file(void)
{
  config_file = cfg_init( gmond_opts, CFGF_NOCASE );

  init_validate_funcs();  /* in config.c */

  switch( cfg_parse( config_file, args_info.conf_arg ) )
    {
    case CFG_FILE_ERROR:
      /* Unable to open file so we'll go with the configuration defaults */
      fprintf(stderr,"Configuration file '%s' not found.\n", args_info.conf_arg);
      if(args_info.conf_given)
	{
	  /* If they explicitly stated a configuration file exit with error... */
	  exit(1);
	}
      /* .. otherwise use our default configuration */
      fprintf(stderr,"Using defaults.\n");
      if(cfg_parse_buf(config_file, DEFAULT_CONFIGURATION) == CFG_PARSE_ERROR)
	{
	  fprintf(stderr,"Your default configuration buffer failed to parse. Exiting.\n");
          exit(1);
	}
      break;
    case CFG_PARSE_ERROR:
      fprintf(stderr,"Parse error for '%s'\n", args_info.conf_arg);
      exit(1);
    case CFG_SUCCESS:
      break;
    default:
      /* I have no clue whats goin' on here... */
      exit(1);
    }
  /* Free memory for this configuration file at exit */
  atexit(cleanup_configuration_file);
}

static void
cleanup_apr_library( void )
{
  apr_pool_destroy(global_context);
  apr_terminate();
}

static void
initialize_apr_library( void )
{
  apr_status_t status;

  /* Initialize apr */
  status = apr_initialize();
  if(status != APR_SUCCESS)
    {
      fprintf(stderr,"Unable to initialize APR library. Exiting.\n");
      exit(1);
    }

  /* Create the global context */
  status = apr_pool_create( &global_context, NULL );
  if(status != APR_SUCCESS)
    {
      fprintf(stderr,"Unable to create global context. Exiting.\n");
      exit(1);
    }

  atexit(cleanup_apr_library);
}

static void
daemonize_if_necessary( char *argv[] )
{
  int should_daemonize;
  cfg_t *tmp;
  tmp = cfg_getsec( config_file, "behavior");
  should_daemonize = cfg_getbool( tmp, "daemonize");

  /* Commandline for debug_level trumps configuration file behaviour ... */
  if (args_info.debug_given) 
    {
      debug_level = args_info.debug_arg;
    }
  else
    {
      debug_level = cfg_getint ( tmp, "debug_level");
    }

  /* Daemonize if needed */
  if(!args_info.foreground_flag && should_daemonize && !debug_level)
    {
      apr_proc_detach(1);
    }
}

static void
setuid_if_necessary( void )
{
  cfg_t *tmp;
  int setuid;
  char *user;

  tmp    = cfg_getsec( config_file, "behavior");
  setuid = cfg_getbool( tmp, "setuid" );
  if(setuid)
    {
      user = cfg_getstr(tmp, "user" );
      become_a_nobody(user);
    }
}

static void
process_deaf_mute_mode( void )
{
  cfg_t *tmp = cfg_getsec( config_file, "behavior");
  deaf =       cfg_getbool( tmp, "deaf");
  mute =       cfg_getbool( tmp, "mute");
  if(deaf && mute)
    {
      fprintf(stderr,"Configured to run both deaf and mute. Nothing to do. Exiting.\n");
      exit(1);
    }
}

static void
setup_udp_recv_pollset( void )
{
  apr_status_t status;
  /* We will open sockets to listen for messages */
  int i, num_udp_recv_channels = cfg_size( config_file, "udp_recv_channel");

  /* Create my UDP recv pollset */
  apr_pollset_create(&udp_recv_pollset, num_udp_recv_channels, global_context, 0);

  /* Create my UDP recv access control array */
  udp_recv_acl_array = apr_array_make( global_context, num_udp_recv_channels,
                                   sizeof(apr_ipsubnet_t *));

  for(i = 0; i< num_udp_recv_channels; i++)
    {
      cfg_t *udp_recv_channel;
      char *mcast_join, *mcast_if, *bindaddr, *protocol, *allow_ip, *allow_mask;
      int port;
      apr_socket_t *socket = NULL;
      apr_ipsubnet_t *ipsub = NULL;
      apr_pollfd_t socket_pollfd;
      apr_pool_t *pool = NULL;

      udp_recv_channel = cfg_getnsec( config_file, "udp_recv_channel", i);
      mcast_join     = cfg_getstr( udp_recv_channel, "mcast_join" );
      mcast_if       = cfg_getstr( udp_recv_channel, "mcast_if" );
      port           = cfg_getint( udp_recv_channel, "port");
      bindaddr       = cfg_getstr( udp_recv_channel, "bind");
      protocol       = cfg_getstr( udp_recv_channel, "protocol");
      allow_ip       = cfg_getstr( udp_recv_channel, "allow_ip");
      allow_mask     = cfg_getstr( udp_recv_channel, "allow_mask");

      debug_msg("udp_recv_channel mcast_join=%s mcast_if=%s port=%d bind=%s protocol=%s\n",
		  mcast_join? mcast_join:"NULL", 
		  mcast_if? mcast_if:"NULL", port,
		  bindaddr? bindaddr: "NULL",
		  protocol? protocol:"NULL");

      /* Create a sub-pool for this channel */
      apr_pool_create(&pool, global_context);

      if( mcast_join )
	{
	  /* Listen on the specified multicast channel */
	  socket = create_mcast_server(pool, mcast_join, port, bindaddr, mcast_if );
	  if(!socket)
	    {
	      fprintf(stderr,"Error creating multicast server mcast_join=%s port=%d mcast_if=%s. Exiting.\n",
		      mcast_join? mcast_join: "NULL", port, mcast_if? mcast_if:"NULL");
	      exit(1);
	    }

	}
      else
	{
	  /* Create a UDP server */
          socket = create_udp_server( pool, port, bindaddr );
          if(!socket)
            {
              fprintf(stderr,"Error creating UDP server on port %d bind=%s. Exiting.\n",
		      port, bindaddr? bindaddr: "unspecified");
	      exit(1);
	    }
	}

      /* Build the socket poll file descriptor structure */
      socket_pollfd.desc_type   = APR_POLL_SOCKET;
      socket_pollfd.reqevents   = APR_POLLIN;
      socket_pollfd.desc.s      = socket;
      socket_pollfd.client_data = protocol;

      /* Add the socket to the pollset */
      status = apr_pollset_add(udp_recv_pollset, &socket_pollfd);
      if(status != APR_SUCCESS)
	{
	  fprintf(stderr,"Failed to add socket to pollset. Exiting.\n");
	  exit(1);
	}

      /* Save the ACL information */
      if(allow_ip)
	{
	  status = apr_ipsubnet_create(&ipsub, allow_ip, allow_mask, global_context);
	  if(status != APR_SUCCESS)
	    {
	      fprintf(stderr,"Unable to build ACL for ip=%s mask=%s. Exiting.\n",
		      allow_ip, allow_mask);
	      exit(1);
	    }
	}
      /* ipsub of NULL means no acl in effect */
      *(apr_ipsubnet_t **)apr_array_push(udp_recv_acl_array) = ipsub;
    }
}

static Ganglia_host_data *
Ganglia_host_data_get( char *remoteip, apr_sockaddr_t *sa, Ganglia_message *fullmsg)
{
  apr_status_t status;
  apr_pool_t *pool;
  Ganglia_host_data *hostdata;
  char *hostname = NULL;

  if(!remoteip || !sa || !fullmsg)
    {
      return NULL;
    }

  hostdata =  (Ganglia_host_data *)apr_hash_get( hosts, remoteip, APR_HASH_KEY_STRING );
  if(!hostdata)
    {
      /* Lookup the hostname or use the proxy information if available */
      if( !hostname )
	{
	  /* We'll use the resolver to find the hostname */
          status = apr_getnameinfo(&hostname, sa, 0);
          if(status != APR_SUCCESS)
	    {
	      /* If hostname lookup fails.. set it to the ip */
	      hostname = remoteip;
	    }
	}

      /* This is the first time we've heard from this host.. create a new pool */
      status = apr_pool_create( &pool, global_context );
      if(status != APR_SUCCESS)
	{
	  return NULL;
	}

      /* Malloc the hostdata_t from the new pool */
      hostdata = apr_pcalloc( pool, sizeof( Ganglia_host_data ));
      if(!hostdata)
	{
	  return NULL;
	}

      /* Save the pool address for later.. freeing this pool free everthing
       * for this particular host */
      hostdata->pool = pool;

      /* Save the hostname */
      hostdata->hostname = apr_pstrdup( pool, hostname );

      /* Dup the remoteip (it will be freed later) */
      hostdata->ip =  apr_pstrdup( pool, remoteip);

      /* Set the timestamps */
      hostdata->first_heard_from = hostdata->last_heard_from = apr_time_now();

      /* Create a hash for the metric data */
      hostdata->metrics = apr_hash_make( pool );
      if(!hostdata->metrics)
	{
	  apr_pool_destroy(pool);
	  return NULL;
	}

      /* Create a hash for the gmetric data */
      hostdata->gmetrics = apr_hash_make( pool );
      if(!hostdata->gmetrics)
	{
	  apr_pool_destroy(pool);
	  return NULL;
	}

      /* Save this host data to the "hosts" hash */
      apr_hash_set( hosts, hostdata->ip, APR_HASH_KEY_STRING, hostdata); 
    }
  else
    {
      /* We already have this host in our "hosts" hash update timestamp */
      hostdata->last_heard_from = apr_time_now();
    }


  /* TODO: Capture special metrics here */
  if(fullmsg->id == metric_location)
    {
      return NULL;
    }
  if(fullmsg->id == metric_heartbeat)
    {
      /* nothing more needs to be done. we handled the timestamps above. */
      return NULL;
    }
  if(fullmsg->id == metric_gexec)
    {
      return NULL;
    }

  return hostdata;
}

static void
Ganglia_message_save( Ganglia_host_data *host, Ganglia_message *message )
{
  Ganglia_metric_data *saved = NULL;

  if(!host || !message)
    return;

  /* Search for the message id in the current host metric hash */
  if(message->id == metric_user_defined)
    {
      /* This is a gmetric message .. ignore for now */
      return;
    }

  saved = (Ganglia_metric_data *)apr_hash_get( host->metrics, &(message->id), sizeof(message->id) );
  if(!saved)
    {
      /* This is a new metric sent from this host... allocate space for this data */
      saved = apr_pcalloc( host->pool, sizeof(Ganglia_metric_data));
      if(!saved)
	{
	  /* no memory */
	  return;
	}
      /* Copy the data */
      memcpy( &(saved->message), message, sizeof(Ganglia_message));
    }
  else
    {
      /* This is a metric update.
       * Free the old data (note: currently this is only necessary for string data) */
      xdr_free((xdrproc_t)xdr_Ganglia_message, (char *)&(saved->message));
      /* Copy the new data in */
      memcpy(&(saved->message), message, sizeof(Ganglia_message));
    }

  /* Timestamp */
  saved->last_heard_from = apr_time_now();

  /* Save the data to the metric hash */
  apr_hash_set( host->metrics, &(message->id), sizeof(message->id), saved ); 
}

static void
poll_udp_recv_channels(apr_interval_time_t timeout)
{
  apr_status_t status;
  const apr_pollfd_t *descs = NULL;
  apr_int32_t num = 0;

  /* Poll for data with given timeout */
  status = apr_pollset_poll(udp_recv_pollset, timeout, &num, &descs);
  if(status != APR_SUCCESS)
    return;

  if(num>0)
    {
      int i;

      /* We have data to read */
      for(i=0; i< num; i++)
        {
	  apr_socket_t *socket;
	  apr_sockaddr_t *remotesa = NULL;
	  char  *protocol, remoteip[256];
	  apr_ipsubnet_t *ipsub;
	  char buf[max_udp_message_len];
	  apr_size_t len = max_udp_message_len;

	  socket         = descs[i].desc.s;
	  /* We could also use the apr_socket_data_get/set() functions
	   * to have per socket user data .. see APR docs */
	  protocol       = descs[i].client_data;

	  apr_socket_addr_get(&remotesa, APR_REMOTE, socket);

	  /* Grab the data */
	  status = apr_socket_recvfrom(remotesa, socket, 0, buf, &len);
	  if(status != APR_SUCCESS)
	    {
	      continue;
	    }	  

	  /* This function is in ./lib/apr_net.c and not APR. The
	   * APR counterpart is apr_sockaddr_ip_get() but we don't 
	   * want to malloc memory evertime we call this */
	  apr_sockaddr_ip_buffer_get(remoteip, 256, remotesa);

	  /* Check the ACL (we can make this better later) */
	  ipsub = ((apr_ipsubnet_t **)(udp_recv_acl_array->elts))[i];
	  if(ipsub)
	    {
	      if(!apr_ipsubnet_test( ipsub, remotesa))
		{
		  debug_msg("Ignoring data from %s\n", remoteip);
		  continue; /* to the next channel that needs read */
		}
	    }

	  if(!strcasecmp(protocol, "xdr"))
	    {
	      XDR x;
	      Ganglia_message msg;
	      Ganglia_host_data *hostdata = NULL;

              /* Create the XDR receive stream */
	      xdrmem_create(&x, buf, max_udp_message_len, XDR_DECODE);

              /* Flush the data... */
	      memset( &msg, 0, sizeof(Ganglia_message));

	      /* Read the gangliaMessage from the stream */
	      if(!xdr_Ganglia_message(&x, &msg))
                {	
	          continue;
	        }

	      /* Process the host information and get the Ganglia_host_data.
	       * We call this _after_ we process the content of the message
	       * because the newer message format allows for proxy information
	       * to be sent.  NOTE: The ACL test above looks at the IP header
	       * and not the proxy information provided. */
              hostdata = Ganglia_host_data_get( remoteip, remotesa, &msg);
	      if(!hostdata)
		{
		  /* Processing of this message is finished ... */
		  xdr_free((xdrproc_t)xdr_Ganglia_message, (char *)&msg);
		  continue;
		}

	      /* Save the message from this particular host */
	      Ganglia_message_save( hostdata, &msg );

#if 0
	      /* Save the message to the hostdata metric hash */
	      old_metric = Ganglia_old_metric_get( msg.id );
	      if(old_metric)
		{
		  /* Move this data into a newer format (later) */
		  debug_msg("%s\t(%s)\t=>\t%s", hostdata->ip, hostdata->hostname, old_metric->name);
		}
#endif

	      /*
	      xdr_free((xdrproc_t)xdr_Ganglia_message, (char *)&msg);
	      */
	      
	      /* If I want to find out how much data I decoded 
	      decoded = xdr_getpos(&x); */
	    }
        } 
    }
}

static void
setup_udp_send_array( void )
{
  int i, num_udp_send_channels = cfg_size( config_file, "udp_send_channel");

  if(num_udp_send_channels <= 0)
    return;

  /* Create my UDP send array */
  udp_send_array = apr_array_make( global_context, num_udp_send_channels, 
				   sizeof(apr_socket_t *));

  for(i = 0; i< num_udp_send_channels; i++)
    {
      cfg_t *udp_send_channel;
      char *mcast_join, *mcast_if, *protocol, *ip;
      int port;
      apr_socket_t *socket = NULL;
      apr_pool_t *pool = NULL;

      udp_send_channel = cfg_getnsec( config_file, "udp_send_channel", i);
      ip             = cfg_getstr( udp_send_channel, "ip" );
      mcast_join     = cfg_getstr( udp_send_channel, "mcast_join" );
      mcast_if       = cfg_getstr( udp_send_channel, "mcast_if" );
      port           = cfg_getint( udp_send_channel, "port");
      protocol       = cfg_getstr( udp_send_channel, "protocol");

      debug_msg("udp_send_channel mcast_join=%s mcast_if=%s ip=%s port=%d protocol=%s\n",
		  mcast_join? mcast_join:"NULL", 
		  mcast_if? mcast_if:"NULL",
		  ip,
		  port, 
		  protocol? protocol:"NULL");

      /* Create a subpool */
      apr_pool_create(&pool, global_context);

      /* Join the specified multicast channel */
      if( mcast_join )
	{
	  /* We'll be listening on a multicast channel */
	  socket = NULL; /* create_mcast_client(...); */
	  if(!socket)
	    {
	      fprintf(stderr,"Unable to join multicast channel %s:%d. Exiting\n",
		      mcast_join, port);
	      exit(1);
	    }
	}
      else
	{
          /* Create a UDP socket */
          socket = create_udp_client( pool, ip, port );
          if(!socket)
            {
              fprintf(stderr,"Unable to create UDP client for %s:%d. Exiting.\n",
		      ip? ip: "NULL", port);
	      exit(1);
	    }
	}

      /* Add the socket to the array */
      *(apr_socket_t **)apr_array_push(udp_send_array) = socket;
    }
}

static void
setup_tcp_accept_pollset( void )
{
  apr_status_t status;
  int i, num_tcp_accept_channels = cfg_size( config_file, "tcp_accept_channel");

  /* Create my TCP accept pollset */
  apr_pollset_create(&tcp_accept_pollset, num_tcp_accept_channels, global_context, 0);

  /* Create my TCP accept ACL array */
  tcp_accept_acl_array = apr_array_make( global_context, num_tcp_accept_channels,
					 sizeof(apr_ipsubnet_t *));

  for(i=0; i< num_tcp_accept_channels; i++)
    {
      cfg_t *tcp_accept_channel = cfg_getnsec( config_file, "tcp_accept_channel", i);
      char *bindaddr, *protocol, *allow_ip, *allow_mask, *interface;
      int port;
      apr_socket_t *socket = NULL;
      apr_ipsubnet_t *ipsub = NULL;
      apr_pollfd_t socket_pollfd;
      apr_pool_t *pool = NULL;

      port           = cfg_getint( tcp_accept_channel, "port");
      bindaddr       = cfg_getstr( tcp_accept_channel, "bind");
      protocol       = cfg_getstr( tcp_accept_channel, "protocol");
      allow_ip       = cfg_getstr( tcp_accept_channel, "allow_ip");
      allow_mask     = cfg_getstr( tcp_accept_channel, "allow_mask");
      interface      = cfg_getstr( tcp_accept_channel, "interface"); 

      debug_msg("tcp_accept_channel bind=%s port=%d protocol=%s\n",
		  bindaddr? bindaddr: "NULL", port,
		  protocol? protocol:"NULL");

      /* Create a subpool context */
      apr_pool_create(&pool, global_context);

      /* Create the socket for the channel */
      socket = create_tcp_server(pool, port, bindaddr, interface);
      if(!socket)
	{
	  fprintf(stderr,"Unable to create tcp_accept_channel. Exiting.\n");
	  exit(1);
	}

      /* Build the socket poll file descriptor structure */
      socket_pollfd.desc_type   = APR_POLL_SOCKET;
      socket_pollfd.reqevents   = APR_POLLIN;
      socket_pollfd.desc.s      = socket;
      socket_pollfd.client_data = protocol;

      /* Add the socket to the pollset */
      status = apr_pollset_add(tcp_accept_pollset, &socket_pollfd);
      if(status != APR_SUCCESS)
         {
            fprintf(stderr,"Failed to add socket to pollset. Exiting.\n");
            exit(1);
         }

      /* Save the ACL information */
      if(allow_ip)
	{
	  status = apr_ipsubnet_create(&ipsub, allow_ip, allow_mask, pool);
	  if(status != APR_SUCCESS)
	    {
	      fprintf(stderr,"Unable to build ACL for ip=%s mask=%s. Exiting.\n",
		      allow_ip, allow_mask);
	      exit(1);
	    }
	}

      /* ipsub of NULL means no acl in effect */
      *(apr_ipsubnet_t **)apr_array_push(tcp_accept_acl_array) = ipsub;
    }
}

static apr_status_t
print_xml_header( apr_socket_t *client )
{
  apr_status_t status;
  apr_size_t len = strlen(DTD);
  char gangliaxml[128];
  char clusterxml[1024];
  static int  clusterinit = 0;
  static char *name = NULL;
  static char *owner = NULL;
  static char *latlong = NULL;
  static char *url = NULL;
  apr_time_t now = apr_time_now();

  status = apr_send( client, DTD, &len );
  if(status != APR_SUCCESS)
    return status;

  len = apr_snprintf( gangliaxml, 128, "<GANGLIA_XML VERSION=\"%s\" SOURCE=\"gmond\">\n",
		      VERSION);
  status = apr_send( client, gangliaxml, &len);
  if(status != APR_SUCCESS)
    return status;

  if(!clusterinit)
    {
      cfg_t *cluster = cfg_getsec(config_file, "cluster");
      name    = cfg_getstr( cluster, "name" );
      owner   = cfg_getstr( cluster, "owner" );
      latlong = cfg_getstr( cluster, "latlong" );
      url     = cfg_getstr( cluster, "url" );
      clusterinit = 1;
    }

  len = apr_snprintf( clusterxml, 1024, 
	"<CLUSTER NAME=\"%s\" LOCALTIME=\"%d\" OWNER=\"%s\" LATLONG=\"%s\" URL=\"%s\">\n", 
		      name?name:"unspecified", 
		      (int)(now / APR_USEC_PER_SEC),
		      owner?owner:"unspecified", 
		      latlong?latlong:"unspecified",
		      url?url:"unspecified");

  return apr_send( client, clusterxml, &len);
}

static apr_status_t
print_xml_footer( apr_socket_t *client )
{
  apr_size_t len = 26; 
  return apr_send( client, "</CLUSTER>\n</GANGLIA_XML>\n", &len);
}

static apr_status_t
print_host_start( apr_socket_t *client, Ganglia_host_data *hostinfo)
{
  apr_size_t len;
  char hostxml[1024]; /* for <HOST></HOST> */
  apr_time_t now = apr_time_now();
  int tn = (now - hostinfo->last_heard_from) / APR_USEC_PER_SEC;

  len = apr_snprintf(hostxml, 1024, 
           "<HOST NAME=\"%s\" IP=\"%s\" REPORTED=\"%d\" TN=\"%d\" TMAX=\"%d\" DMAX=\"%d\" LOCATION=\"%s\" GMOND_STARTED=\"%d\">\n",
                     hostinfo->hostname, 
		     hostinfo->ip, 
		     (int)(hostinfo->last_heard_from / APR_USEC_PER_SEC),
		     tn,
		     20, /*tmax for now is always 20 */
		     0 /*dmax*/,
		     "unspecified", /*location*/
		     0 /*gmond_started*/);

  return apr_send(client, hostxml, &len);
}

/* NOT THREAD SAFE */
static char *
host_metric_type( Ganglia_value_types type)
{
  switch(type)
    {
    case GANGLIA_VALUE_UNKNOWN:
      return "unknown";
    case GANGLIA_VALUE_STRING:
      return "string";
    case GANGLIA_VALUE_UNSIGNED_SHORT:
      return "uint16";
    case GANGLIA_VALUE_SHORT:
      return "int16";
    case GANGLIA_VALUE_UNSIGNED_INT:
      return "uint32";
    case GANGLIA_VALUE_INT:
      return "int32";
    case GANGLIA_VALUE_FLOAT:
      return "float";
    case GANGLIA_VALUE_DOUBLE:
      return "double";
    }
  return "undef";
}

/* NOT THREAD SAFE */
static char *
host_metric_value( Ganglia_old_metric *metric, Ganglia_message *message )
{
  static char value[1024];
  if(!metric||!message)
    {
      return "unknown";
    }

  switch(metric->type)
    {
    case GANGLIA_VALUE_UNKNOWN:
      return "unknown";
    case GANGLIA_VALUE_STRING:
      return message->Ganglia_message_u.str;
    case GANGLIA_VALUE_UNSIGNED_SHORT:
      apr_snprintf(value, 1024, metric->fmt, message->Ganglia_message_u.u_short);
      return value;
    case GANGLIA_VALUE_SHORT:
      apr_snprintf(value, 1024, metric->fmt, message->Ganglia_message_u.u_short);
      return value;
    case GANGLIA_VALUE_UNSIGNED_INT:
      apr_snprintf(value, 1024, metric->fmt, message->Ganglia_message_u.u_int);
      return value;
    case GANGLIA_VALUE_INT:
      apr_snprintf(value, 1024, metric->fmt, message->Ganglia_message_u.u_int);
      return value;
    case GANGLIA_VALUE_FLOAT:
      apr_snprintf(value, 1024, metric->fmt, message->Ganglia_message_u.f);
      return value;
    case GANGLIA_VALUE_DOUBLE:
      apr_snprintf(value, 1024, metric->fmt, message->Ganglia_message_u.d);
      return value;
    }

  return "unknown";
}

static apr_status_t
print_host_metric( apr_socket_t *client, Ganglia_metric_data *data )
{
  Ganglia_old_metric *metric;
  char metricxml[1024];
  apr_size_t len;
  apr_time_t now;

  metric = Ganglia_old_metric_get( data->message.id );
  if(!metric)
    return APR_SUCCESS;

  now = apr_time_now();

  len = apr_snprintf(metricxml, 1024,
          "<METRIC NAME=\"%s\" VAL=\"%s\" TYPE=\"%s\" UNITS=\"%s\" TN=\"%d\" TMAX=\"%d\" DMAX=\"0\" SLOPE=\"%s\" SOURCE=\"gmond\"/>\n",
	  metric->name,
	  host_metric_value( metric, &(data->message) ),
	  host_metric_type( metric->type ),
	  metric->units? metric->units: "",
	  (int)((now - data->last_heard_from) / APR_USEC_PER_SEC),
	  metric->step,
	  metric->slope );

  return apr_send(client, metricxml, &len);
}


static apr_status_t
print_host_end( apr_socket_t *client)
{
  apr_size_t len = 8;
  return apr_send(client, "</HOST>\n", &len); 
}

static void
poll_tcp_accept_channels(apr_interval_time_t timeout)
{
  apr_status_t status;
  const apr_pollfd_t *descs = NULL;
  apr_int32_t num = 0;

  /* Poll for data with given timeout */
  status = apr_pollset_poll(tcp_accept_pollset, timeout, &num, &descs);
  if(status != APR_SUCCESS)
    return;

  if(num>0)
    {
      int i;

      /* We have data to read */
      for(i=0; i< num; i++)
        {
	  apr_socket_t *client, *server;
	  apr_sockaddr_t *remotesa = NULL;
	  char  *protocol, remoteip[256];
	  apr_ipsubnet_t *ipsub;
	  apr_pool_t *client_context = NULL;

	  server         = descs[i].desc.s;
	  /* We could also use the apr_socket_data_get/set() functions
	   * to have per socket user data .. see APR docs */
	  protocol       = descs[i].client_data;

	  /* Create a context for the client connection */
	  apr_pool_create(&client_context, global_context);

	  /* Accept the connection */
	  status = apr_accept(&client, server, client_context);
	  if(status != APR_SUCCESS)
	    {
	      goto close_accept_socket;
	    }

	  apr_socket_addr_get(&remotesa, APR_REMOTE, client);
	  /* This function is in ./lib/apr_net.c and not APR. The
	   * APR counterpart is apr_sockaddr_ip_get() but we don't 
	   * want to malloc memory evertime we call this */
	  apr_sockaddr_ip_buffer_get(remoteip, 256, remotesa);

	  /* Check the ACL (we can make this better later) */
	  ipsub = ((apr_ipsubnet_t **)(tcp_accept_acl_array->elts))[i];
	  if(ipsub)
	    {
	      if(!apr_ipsubnet_test( ipsub, remotesa))
		{
		  debug_msg("Ignoring connection from %s\n", remoteip);
		  goto close_accept_socket;
		}
	    }

	  /* At this point send data over the socket */
	  if(!strcasecmp(protocol, "xml"))
	    {
	      apr_status_t status;
	      apr_hash_index_t *hi, *metric_hi;
	      void *val;

	      /* Print the DTD, GANGLIA_XML and CLUSTER tags */
	      status = print_xml_header(client);
	      if(status != APR_SUCCESS)
		goto close_accept_socket;

	      /* Walk the host hash */
	      for(hi = apr_hash_first(client_context, hosts);
		  hi;
		  hi = apr_hash_next(hi))
		{
		  apr_hash_this(hi, NULL, NULL, &val);
		  status = print_host_start(client, (Ganglia_host_data *)val);
	          if(status != APR_SUCCESS)
	            {
                      goto close_accept_socket;
	            }

		  /* Send the metric info for this particular host */
		  for(metric_hi = apr_hash_first(client_context, ((Ganglia_host_data *)val)->metrics);
		      metric_hi;
		      metric_hi = apr_hash_next(metric_hi))
		    {
		      void *metric;
		      apr_hash_this(metric_hi, NULL, NULL, &metric);

		      /* Print each of the metrics for a host ... */
		      if(print_host_metric(client, metric) != APR_SUCCESS)
			{
			  goto close_accept_socket;
			}
		    }

		  /* Close the host tag */
		  status = print_host_end(client);
		  if(status != APR_SUCCESS)
		    {
		      goto close_accept_socket;
		    }
		}

	      /* Close the CLUSTER and GANGLIA_XML tags */
	      print_xml_footer(client);
	    }

	  /* Close down the accepted socket */
      close_accept_socket:
	  apr_shutdown(client, APR_SHUTDOWN_READ);
	  apr_socket_close(client);
	  apr_pool_destroy(client_context);
        } 
    }
}

/* This function will send a datagram to every udp_send_channel specified */
static int
udp_send_message( char *buf, int len )
{
  apr_status_t status;
  int i;
  int num_errors = 0;
  apr_size_t size;

  /* Return if we have no data or we're muted */
  if(!buf || mute)
    return 1;

  for(i=0; i< udp_send_array->nelts; i++)
    {
      apr_socket_t *socket = ((apr_socket_t **)(udp_send_array->elts))[i];
      size   = len;
      status = apr_socket_send( socket, buf, &size );
      if(status != APR_SUCCESS)
	{
	  num_errors++;
	}
    }
  return num_errors;
}

static int
tcp_send_message( char *buf, int len )
{
  /* Mirror of UDP send message for TCP channels */
  return 0;
}

static Ganglia_metric_callback *
Ganglia_metric_cb_define(  char *name, g_val_t (*cb)(void))
{
  Ganglia_metric_callback *metric = apr_pcalloc( global_context, sizeof(Ganglia_metric_callback));
  if(!metric)
    return NULL;

  metric->name = apr_pstrdup( global_context, name );
  if(!metric->name)
    return NULL;

  metric->cb = cb;
  apr_hash_set( metric_callbacks, metric->name, APR_HASH_KEY_STRING, metric);
  return metric;
}

/* This function imports the metrics from libmetrics right now but in the future
 * we could easily do this via DSO. */
static void
setup_metric_callbacks( void )
{
  /* Initialize the libmetrics library in ./srclib/libmetrics */
  libmetrics_init();

  /* Create the metric_callbacks hash */
  metric_callbacks = apr_hash_make( global_context );

  /* All platforms support these metrics */
  Ganglia_metric_cb_define("cpu_num",        cpu_num_func);
  Ganglia_metric_cb_define("cpu_speed",      cpu_speed_func);
  Ganglia_metric_cb_define("mem_total",      mem_total_func);
  Ganglia_metric_cb_define("swap_total",     swap_total_func);
  Ganglia_metric_cb_define("boottime",       boottime_func);
  Ganglia_metric_cb_define("sys_clock",      sys_clock_func);
  Ganglia_metric_cb_define("machine_type",   machine_type_func);
  Ganglia_metric_cb_define("os_name",        os_name_func);
  Ganglia_metric_cb_define("os_release",     os_release_func);
  Ganglia_metric_cb_define("mtu",            mtu_func);
  Ganglia_metric_cb_define("cpu_user",       cpu_user_func);
  Ganglia_metric_cb_define("cpu_nice",       cpu_nice_func);
  Ganglia_metric_cb_define("cpu_system",     cpu_system_func);
  Ganglia_metric_cb_define("cpu_idle",       cpu_idle_func);
  Ganglia_metric_cb_define("cpu_aidle",      cpu_aidle_func);
  Ganglia_metric_cb_define("bytes_in",       bytes_in_func);
  Ganglia_metric_cb_define("bytes_out",      bytes_out_func);
  Ganglia_metric_cb_define("pkts_in",        pkts_in_func);
  Ganglia_metric_cb_define("pkts_out",       pkts_out_func);
  Ganglia_metric_cb_define("disk_total",     disk_total_func);
  Ganglia_metric_cb_define("disk_free",      disk_free_func);
  Ganglia_metric_cb_define("part_max_used",  part_max_used_func);
  Ganglia_metric_cb_define("load_one",       load_one_func);
  Ganglia_metric_cb_define("load_five",      load_five_func);
  Ganglia_metric_cb_define("load_fifteen",   load_fifteen_func);
  Ganglia_metric_cb_define("proc_run",       proc_run_func);
  Ganglia_metric_cb_define("proc_total",     proc_total_func);
  Ganglia_metric_cb_define("mem_free",       mem_free_func);
  Ganglia_metric_cb_define("mem_shared",     mem_shared_func);
  Ganglia_metric_cb_define("mem_buffers",    mem_buffers_func);
  Ganglia_metric_cb_define("mem_cached",     mem_cached_func);
  Ganglia_metric_cb_define("swap_free",      swap_free_func);
}
 
int
process_collection_groups( void )
{
  int i, num_collection_groups = cfg_size( config_file, "collection_group" );

  for(i=0; i< num_collection_groups; i++)
    {
      int j, num_metrics;

      cfg_t *group = cfg_getnsec( config_file, "collection_group", i);
      char *name   = cfg_getstr( group, "name");
      num_metrics  = cfg_size( group, "metric" );

      for(j=0; j< num_metrics; j++)
	{
          cfg_t *metric = cfg_getnsec( group, "metric", j );

	  /* Process the data for this metric */
	  

	}
    }

  return 2;
}

int
main ( int argc, char *argv[] )
{
  apr_interval_time_t now, stop;
  int next_collection;

  /* Mark the time this gmond started */
  started = apr_time_now();

  if (cmdline_parser (argc, argv, &args_info) != 0)
    exit(1) ;

  if(args_info.default_config_flag)
    {
      fprintf(stdout, DEFAULT_CONFIGURATION);
      fflush( stdout );
      exit(0);
    }

  process_configuration_file();

  daemonize_if_necessary( argv );
  
  /* Initializes the apr library in ./srclib/apr */
  initialize_apr_library();

  /* Collect my hostname */
  apr_gethostname( myname, APRMAXHOSTLEN+1, global_context);

  apr_signal( SIGPIPE, SIG_IGN );

  setuid_if_necessary();

  process_deaf_mute_mode();

  if(!deaf)
    {
      setup_udp_recv_pollset();
      setup_tcp_accept_pollset();
    }

  if(!mute)
    {
      setup_metric_callbacks();
      setup_udp_send_array();
    }

  /* Create the host hash table */
  hosts = apr_hash_make( global_context );

  next_collection = 0;
  for(;;)
    {
      now  = apr_time_now();
      stop = now + (next_collection* APR_USEC_PER_SEC);
      /* Read data until we need to collect/write data */
      for(; now < stop ;)
	{
          if(!deaf)
	    {
	      poll_tcp_accept_channels(0);
              poll_udp_recv_channels(stop - now);  
	    }
	  now = apr_time_now();
	}

      if(!mute)
	{
	  next_collection = process_collection_groups();
	}
      else
	{
	  next_collection = 3600; /* if we're mute.
				  set the default timeout large...*/
	}
    }

  return 0;
}
