/*
$Id$
    OWFS -- One-Wire filesystem
    OWHTTPD -- One-Wire Web Server
    Written 2003 Paul H Alfille
	email: palfille@earthlink.net
	Released under the GPL
	See the header file: ow.h for full attribution
	1wire/iButton system from Dallas Semiconductor
*/

#include <config.h>
#include "owfs_config.h"
#include "ow.h"
#include "ow_counters.h"
#include "ow_connection.h"

/* Telnet handling concepts from Jerry Scharf:

You should request the number of bytes you expect. When you scan it,
start looking for FF FA codes. If that shows up, see if the F0 is in the
read buffer. If so, move the char pointer back to where the FF point was
and ask for the rest of the string (expected - FF...F0 bytes.) If the F0
isn't in the read buffer, start reading byte by byte into a separate
variable looking for F0. Once you find that, then do the move the
pointer and read the rest piece as above. Finally, don't forget to scan
the rest of the strings for more FF FA blocks. It's most sloppy when the
FF is the last character of the initial read buffer...

You can also scan and remove the patterns FF F1 - FF F9 as these are 2
byte commands that the transmitter can send at any time. It is also
possible that you could see FF FB xx - FF FE xx 3 byte codes, but this
would be in response to FF FA codes that you would send, so that seems
unlikely. Handling these would be just the same as the FF FA codes above.

*/

static struct timeval tvnet = { 0, 300000, };

//static void byteprint( const BYTE * b, int size ) ;
static int LINKE_write(const BYTE * buf, const size_t size, const struct parsedname *pn);
static int LINKE_reset(const struct parsedname *pn);
static int LINKE_next_both(struct device_search *ds, const struct parsedname *pn);
static int LINKE_PowerByte(const BYTE data, BYTE * resp, const UINT delay, const struct parsedname *pn);
static int LINKE_sendback_data(const BYTE * data, BYTE * resp, const size_t len, const struct parsedname *pn);
static void LINKE_setroutines(struct connection_in *in);
static void LINKE_close(struct connection_in *in);

static void LINKE_setroutines(struct connection_in *in)
{
	in->iroutines.detect = LINKE_detect;
	in->iroutines.reset = LINKE_reset;
	in->iroutines.next_both = LINKE_next_both;
	in->iroutines.PowerByte = LINKE_PowerByte;
//    in->iroutines.ProgramPulse = ;
	in->iroutines.sendback_data = LINKE_sendback_data;
//    in->iroutines.sendback_bits = ;
	in->iroutines.select = NULL;
	in->iroutines.reconnect = NULL;
	in->iroutines.close = LINKE_close;
	in->iroutines.transaction = NULL;
	in->iroutines.flags = ADAP_FLAG_no2409path;
	in->bundling_length = LINKE_FIFO_SIZE;
}

#define LINK_string(x)  ((BYTE *)(x))

int LINKE_detect(struct connection_in *in)
{
	struct parsedname pn;

	FS_ParsedName(NULL, &pn);	// minimal parsename -- no destroy needed
	pn.selected_connection = in;
	LEVEL_CONNECT("LinkE detect\n");
	/* Set up low-level routines */
	LINKE_setroutines(in);

	if (in->name == NULL) {
		return -1;
	}
	if (ClientAddr(in->name, in)) {
		return -1;
	}
	if ((pn.selected_connection->file_descriptor = ClientConnect(in)) < 0) {
		return -EIO;
	}

	in->Adapter = adapter_LINK_E;
	in->default_discard = 0 ;
	if (1) {
		BYTE data[6] ;
		size_t read_size ;
		struct timeval tvnetfirst = { Globals.timeout_network, 0, };
		tcp_read(in->file_descriptor, data, 6, &tvnetfirst, &read_size ) ;
	}
	TCP_slurp( in->file_descriptor ) ;
	tcp_read_flush(in->file_descriptor);
	LEVEL_DEBUG("Slurp in initial bytes\n");
	if (LINKE_write(LINK_string(" "), 1, &pn) == 0) {
		char buf[18];
		if (telnet_read((BYTE *) buf, 18, &pn)
			|| strncmp(buf, "Link", 4))
			return -ENODEV;
		in->adapter_name = "Link-Hub-E";
		return 0;
	}
	return -EIO;
}

static int LINKE_reset(const struct parsedname *pn)
{
	BYTE resp[8];
	int ret;

	tcp_read_flush(pn->selected_connection->file_descriptor);

	// Send 'r' reset
	if (LINKE_write(LINK_string("r"), 1, pn) || telnet_read(resp, 4, pn)) {
		return -EIO;
	}

	switch (resp[0]) {

	case 'P':
		pn->selected_connection->AnyDevices = 1;
		ret = BUS_RESET_OK;
		break;

	case 'N':
		pn->selected_connection->AnyDevices = 0;
		ret = BUS_RESET_OK;
		break;

	case 'S':
		ret = BUS_RESET_SHORT;
		break;

	default:
		ret = -EIO;
		break;

	}

	return ret;
}

static int LINKE_next_both(struct device_search *ds, const struct parsedname *pn)
{
	char resp[21];
	int ret;

	if (!pn->selected_connection->AnyDevices) {
		ds->LastDevice = 1;
	}
	if (ds->LastDevice) {
		return -ENODEV;
	}

	++ds->index;
	if (ds->index == 0) {
		if ((ret = LINKE_write(LINK_string("f"), 1, pn)))
			return ret;
	} else {
		if ((ret = LINKE_write(LINK_string("n"), 1, pn)))
			return ret;
	}

	if ((ret = telnet_read(LINK_string(resp), 21, pn))) {
		return ret;
	}

	switch (resp[0]) {
	case '-':
		ds->LastDevice = 1;
	case '+':
		break;
	case 'N':
		pn->selected_connection->AnyDevices = 0;
		return -ENODEV;
	case 'X':
	default:
		return -EIO;
	}

	ds->sn[7] = string2num(&resp[2]);
	ds->sn[6] = string2num(&resp[4]);
	ds->sn[5] = string2num(&resp[6]);
	ds->sn[4] = string2num(&resp[8]);
	ds->sn[3] = string2num(&resp[10]);
	ds->sn[2] = string2num(&resp[12]);
	ds->sn[1] = string2num(&resp[14]);
	ds->sn[0] = string2num(&resp[16]);

	// CRC check
	if (CRC8(ds->sn, 8) || (ds->sn[0] == 0)) {
		/* A minor "error" and should perhaps only return -1 to avoid reconnect */
		return -EIO;
	}

	if ((ds->sn[0] & 0x7F) == 0x04) {
		/* We found a DS1994/DS2404 which require longer delays */
		pn->selected_connection->ds2404_compliance = 1;
	}

	LEVEL_DEBUG("LINKE_next_both SN found: " SNformat "\n", SNvar(ds->sn));
	return 0;
}

//
// Write a string to the serial port
/* return 0=good,
          -EIO = error
   Special processing for the remote hub (add 0x0A)
 */
static int LINKE_write(const BYTE * buf, const size_t size, const struct parsedname *pn)
{
	ssize_t write_or_error;
	Debug_Bytes( "LINK write", buf, size) ;
	write_or_error = write(pn->selected_connection->file_descriptor, buf, size);

	if (write_or_error < 0) {
		ERROR_CONNECT("Trouble writing data to LINK: %s\n", SAFESTRING(pn->selected_connection->name));
		return write_or_error;
	}

	tcdrain(pn->selected_connection->file_descriptor);
	gettimeofday(&(pn->selected_connection->bus_write_time), NULL);

	if (write_or_error < (ssize_t) size) {
		LEVEL_CONNECT("Short write to LINK -- intended %d, sent %d\n", (int) size, (int) write_or_error);
		STAT_ADD1_BUS(e_bus_write_errors, pn->selected_connection);
		return -EIO;
	}
	//printf("Link wrote <%*s>\n",(int)size,buf);
	return 0;
}

static int LINKE_PowerByte(const BYTE data, BYTE * resp, const UINT delay, const struct parsedname *pn)
{
	ASCII buf[3] = "pxx";

	num2string(&buf[1], data);

	if (LINKE_write(LINK_string(buf), 3, pn) || telnet_read(LINK_string(buf), 2, pn)) {
		return -EIO;			// send just the <CR>
	}

	resp[0] = string2num(buf);

	// delay
	UT_delay(delay);

	if (LINKE_write(LINK_string("\r"), 1, pn) || telnet_read(LINK_string(buf), 3, pn)) {
		return -EIO;			// send just the <CR>
	}

	return 0;
}

// _sendback_data
//  Send data and return response block
/* return 0=good
   sendout_data, readin
 */
// Assume buffer length (combuffer) is 1 + 32*2 + 1
static int LINKE_sendback_data(const BYTE * data, BYTE * resp, const size_t size, const struct parsedname *pn)
{
	size_t left = size;
	BYTE buf[66] ;

	if (size == 0) {
		return 0;
	}

	Debug_Bytes( "ELINK sendback send", data, size) ;
	while (left > 0) {
		size_t this_length = (left > 32) ? 32 : left;
		size_t total_length = 2 * this_length + 2;
		buf[0] = 'b';			//put in byte mode
//        printf(">> size=%d, left=%d, i=%d\n",size,left,i);
		bytes2string((char *) &buf[1], &data[size - left], this_length);
		buf[total_length - 1] = '\r';	// take out of byte mode
		if (LINKE_write(buf, total_length, pn) || telnet_read(buf, total_length + 1, pn)) {
			return -EIO;
		}
		string2bytes((char *) buf, &resp[size - left], this_length);
		left -= this_length;
	}
	return 0;
}

static void LINKE_close(struct connection_in *in)
{
	Test_and_Close( &(in->file_descriptor) ) ;
	FreeClientAddr(in);
}
