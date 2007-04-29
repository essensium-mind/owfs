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

/* General Device File format:
    This device file corresponds to a specific 1wire/iButton chip type
	( or a closely related family of chips )

	The connection to the larger program is through the "device" data structure,
	  which must be declared in the acompanying header file.

	The device structure holds the
	  family code,
	  name,
	  device type (chip, interface or pseudo)
	  number of properties,
	  list of property structures, called "filetype".

	Each filetype structure holds the
	  name,
	  estimated length (in bytes),
	  aggregate structure pointer,
	  data format,
	  read function,
	  write funtion,
	  generic data pointer

	The aggregate structure, is present for properties that several members
	(e.g. pages of memory or entries in a temperature log. It holds:
	  number of elements
	  whether the members are lettered or numbered
	  whether the elements are stored together and split, or separately and joined
*/

#include <config.h>
#include "owfs_config.h"
#include "ow_connection.h"

static void Set_OWQ_length( struct one_wire_query * owq ) ;
static int OW_r_crc16(BYTE code, struct one_wire_query * owq, size_t page, size_t pagesize ) ;

static void Set_OWQ_length( struct one_wire_query * owq ) {
	switch( OWQ_pn(owq).ft->format ) {
		case ft_binary:
		case ft_ascii:
		case ft_vascii:
			OWQ_length(owq) = OWQ_size(owq) ;
			break ;
		default:
			break ;
	}
}

/* No CRC -- 0xF0 code */
int OW_r_mem_simple(struct one_wire_query * owq, size_t page, size_t pagesize )
{
	off_t offset = OWQ_offset(owq) + page * pagesize ;
    BYTE p[3] = { 0xF0, LOW_HIGH_ADDRESS(offset), };
	struct transaction_log t[] = {
		TRXN_START,
		{p, NULL, 3, trxn_match},
        {NULL, (BYTE *) OWQ_buffer(owq), OWQ_size(owq), trxn_read},
		TRXN_END,
	};

	Set_OWQ_length(owq) ;
	return BUS_transaction(t, PN(owq));
}

/* read up to end of page to CRC16 -- 0xA5 code */
static int OW_r_crc16(BYTE code, struct one_wire_query * owq, size_t page, size_t pagesize )
{
    off_t offset = OWQ_offset(owq) + page * pagesize ;
    size_t size = OWQ_size(owq) ;
    BYTE p[3 + pagesize + 2] ;
    int rest = pagesize - (offset % pagesize);
    struct transaction_log t[] = {
        TRXN_START,
        {p, NULL, 3, trxn_match,},
        {NULL, &p[3], rest + 2, trxn_read,},
        {p, NULL, 3 + rest + 2, trxn_crc16, } ,
        TRXN_END,
    };

    p[0] = code;
    p[1] = offset & 0xFF;
    p[2] = (offset >> 8) & 0xFF;
    if (BUS_transaction(t, PN(owq)))
        return 1;
    memcpy(OWQ_buffer(owq), &p[3], size);
    Set_OWQ_length( owq) ;
    return 0;
}

/* read up to end of page to CRC16 -- 0xA5 code */
int OW_r_mem_crc16_A5(struct one_wire_query * owq, size_t page, size_t pagesize )
{
    return OW_r_crc16( 0xA5, owq, page, pagesize ) ;
}

/* read up to end of page to CRC16 -- 0xA5 code */
int OW_r_mem_crc16_AA(struct one_wire_query * owq, size_t page, size_t pagesize )
{
    return OW_r_crc16( 0xAA, owq, page, pagesize ) ;
}

/* read up to end of page to CRC16 -- 0xF0 code */
int OW_r_mem_crc16_F0(struct one_wire_query * owq, size_t page, size_t pagesize )
{
    return OW_r_crc16( 0xF0, owq, page, pagesize ) ;
}

/* read up to end of page to CRC16 -- 0xA5 code */
/* Extra 8 bytes, too */
int OW_r_mem_p8_crc16(struct one_wire_query * owq, size_t page, size_t pagesize, BYTE * extra)
{
    off_t offset = OWQ_offset(owq) + page * pagesize ;
    BYTE p[3 + pagesize + 8 + 2] ;
	int rest = pagesize - (offset % pagesize);
	struct transaction_log t[] = {
		TRXN_START,
		{p, NULL, 3, trxn_match,},
		{NULL, &p[3], rest + 8 + 2, trxn_read,},
		{p, NULL, 3 + rest + 8 + 2, trxn_crc16, } ,
		TRXN_END,
	};

	p[0] = 0xA5;
	p[1] = offset & 0xFF;
	p[2] = (offset >> 8) & 0xFF;
    if (BUS_transaction(t, PN(owq) ))
		return 1;
    if (OWQ_buffer(owq)) memcpy(OWQ_buffer(owq), &p[3], OWQ_size(owq) );
    if ( extra ) memcpy(extra, &p[3 + rest], 8);
    Set_OWQ_length( owq ) ;
    return 0;
}
