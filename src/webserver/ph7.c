/* Pi-hole: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  PH7 virtual machine routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
// strncpy()
#include <string.h>
#include "../log.h"
#include "../ph7/ph7.h"
#include "../civetweb/civetweb.h"
#include "ph7.h"
// struct httpsettings
#include "../config.h"
// mmap
#include <sys/mman.h>
// stat
#include <sys/types.h>
#include <sys/stat.h>
// open
#include <fcntl.h>

// Pi-hole PH7 extensions
#define PH7_CORE
#include "ph7_ext/extensions.h"

// PH7 virtual machine engine
static ph7 *pEngine; /* PH7 engine */
static ph7_vm *pVm;  /* Compiled PHP program */

static char *webroot_with_home = NULL;
static char *webroot_with_home_and_scripts = NULL;

static int PH7_error_report(const void *pOutput, unsigned int nOutputLen,
                            void *pUserData /* Unused */)
{
	// Log error message, strip trailing newline character if any
	if(((const char*)pOutput)[nOutputLen-1] == '\n')
		nOutputLen--;
	logg("PH7 error: %.*s", nOutputLen, (const char*)pOutput);
	return PH7_OK;
}

int ph7_handler(struct mg_connection *conn, void *cbdata)
{
	int rc;

	/* Handler may access the request info using mg_get_request_info */
	const struct mg_request_info *req_info = mg_get_request_info(conn);

	// Build full path of PHP script on our machine
	const size_t webroot_len = strlen(httpsettings.webroot);
	const size_t local_uri_len = strlen(req_info->local_uri + 1u); // +1 to skip the initial '/'
	char full_path[webroot_len + local_uri_len + 2];
	strcpy(full_path, httpsettings.webroot);
	full_path[webroot_len] = '/';
	strncpy(full_path + webroot_len + 1u, req_info->local_uri + 1u, local_uri_len);
	full_path[webroot_len + local_uri_len + 1u] = '\0';
	if(config.debug & DEBUG_API)
		logg("Full path of PHP script: %s", full_path);

	// Compile PHP script into byte-code
	// This usually takes only 1-2 msec even for larger scripts on a Raspberry
	// Pi 3, so there is little point in buffering the compiled script
	rc = ph7_compile_file(
		pEngine,   /* PH7 Engine */
		full_path, /* Path to the PHP file to compile */
		&pVm,      /* OUT: Compiled PHP program */
		0          /* IN: Compile flags */
	);

	if( rc != PH7_OK ) // Compile error
	{
		if( rc == PH7_IO_ERR )
		{
			logg("IO error while opening the target file (%s)", full_path);
			// Fall back to HTTP server to handle the 404 event
			return 0;
		}
		else if( rc == PH7_VM_ERR )
		{
			logg("VM initialization error");
			// Mark file as processes - this prevents the HTTP server
			// from printing the raw PHP source code to the user
			return 1;
		}
		else
		{
			logg("Compile error (%d)", rc);

			mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
			          "PHP compilation error, check %s for further details.",
			          FTLfiles.log);

			/* Extract error log */
			const char *zErrLog = NULL;
			int niLen = 0;
			ph7_config(pEngine, PH7_CONFIG_ERR_LOG, &zErrLog, &niLen);
			if( niLen > 0 ){
				/* zErrLog is null terminated */
				logg("PH7 compile error: %s", zErrLog);
			}
			// Mark file as processes - this prevents the HTTP server
			// from printing the raw PHP source code to the user
			return 1;
		}
	}

	// Pass raw HTTP request head to PH7 so it can decode the queries and
	// fill the appropriate arrays such as $_GET, $_POST, $_REQUEST,
	// $_SERVER, etc. Length -1 means PH7 computes the buffer length itself
	ph7_vm_config(pVm, PH7_VM_CONFIG_HTTP_REQUEST, req_info->raw_http_head, -1);

	/* Report script run-time errors */
	ph7_vm_config(pVm, PH7_VM_CONFIG_ERR_REPORT);

	/* Configure include paths */
	ph7_vm_config(pVm, PH7_VM_CONFIG_IMPORT_PATH, webroot_with_home);
	ph7_vm_config(pVm, PH7_VM_CONFIG_IMPORT_PATH, webroot_with_home_and_scripts);

	// Register Pi-hole's PH7 extensions (defined in subdirectory "ph7_ext/")
	for(unsigned int i = 0; i < sizeof(aFunc)/sizeof(aFunc[0]); i++ )
	{
		rc = ph7_create_function(pVm, aFunc[i].zName, aFunc[i].xProc, NULL /* NULL: No private data */);
		if( rc != PH7_OK ){
			logg("Error while registering foreign function %s()", aFunc[i].zName);
		}
	}

	// Execute virtual machine
	rc = ph7_vm_exec(pVm,0);
	if( rc != PH7_OK )
	{
		logg("VM execution error");
		// Mark file as processes - this prevents the HTTP server
		// from printing the raw PHP source code to the user
		return 1;
	}

	// Extract and send the output (if any)
	const void *pOut = NULL;
	unsigned int nLen = 0u;
	rc = ph7_vm_config(pVm, PH7_VM_CONFIG_EXTRACT_OUTPUT, &pOut, &nLen);
	if(nLen > 0)
	{
		mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n");
		mg_write(conn, pOut, nLen);
	}

	// Reset and release the virtual machine
	ph7_vm_reset(pVm);
	ph7_vm_release(pVm);

	// Processed the file
	return 1;
}

void init_ph7(void)
{
	if(ph7_init(&pEngine) != PH7_OK )
	{
		logg("Error while allocating a new PH7 engine instance");
		return;
	}

	// Set an error log consumer callback. This callback will
	// receive all compile-time error messages to 
	ph7_config(pEngine,PH7_VM_CONFIG_OUTPUT, PH7_error_report, NULL /* NULL: No private data */);

	// Prepare include paths
	// var/www/html/admin (may be different due to user configuration)
	const size_t webroot_len = strlen(httpsettings.webroot);
	const size_t webhome_len = strlen(httpsettings.webhome);
	webroot_with_home = calloc(webroot_len + webhome_len + 1u, sizeof(char));
	strcpy(webroot_with_home, httpsettings.webroot);
	strcpy(webroot_with_home + webroot_len, httpsettings.webhome);
	webroot_with_home[webroot_len + webhome_len] = '\0';

	// var/www/html/admin/scripts/pi-hole/php (may be different due to user configuration)
	const char scripts_dir[] = "/scripts/pi-hole/php";
	size_t scripts_dir_len = sizeof(scripts_dir);
	size_t webroot_with_home_len = strlen(webroot_with_home);
	webroot_with_home_and_scripts = calloc(webroot_with_home_len + scripts_dir_len + 1u, sizeof(char));
	strcpy(webroot_with_home_and_scripts, webroot_with_home);
	strcpy(webroot_with_home_and_scripts + webroot_with_home_len, scripts_dir);
	webroot_with_home_and_scripts[webroot_with_home_len + scripts_dir_len] = '\0';
}

void ph7_terminate(void)
{
	ph7_release(pEngine);
	free(webroot_with_home);
	free(webroot_with_home_and_scripts);
}