/*
linphone
Copyright (C) 2012  Belledonne Communications, Grenoble, France

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
#include "sal_impl.h"

static void process_error( SalOp* op) {
	if (op->dir == SalOpDirOutgoing) {
		op->base.root->callbacks.text_delivery_update(op,SalTextDeliveryFailed);
	} else {
		ms_warning("unexpected io error for incoming message on op [%p]",op);
	}
	op->state=SalOpStateTerminated;

}

static void process_io_error(void *user_ctx, const belle_sip_io_error_event_t *event){
	SalOp* op = (SalOp*)user_ctx;
//	belle_sip_object_t* source = belle_sip_io_error_event_get_source(event);
//	if (BELLE_SIP_IS_INSTANCE_OF(source,belle_sip_transaction_t)) {
//		/*reset op to make sure transaction terminated does not need op*/
//		belle_sip_transaction_set_application_data(BELLE_SIP_TRANSACTION(source),NULL);
//	}
	process_error(op);
}
static void process_timeout(void *user_ctx, const belle_sip_timeout_event_t *event) {
	SalOp* op=(SalOp*)user_ctx;
//	belle_sip_client_transaction_t *client_transaction=belle_sip_timeout_event_get_client_transaction(event);
//	belle_sip_server_transaction_t *server_transaction=belle_sip_timeout_event_get_server_transaction(event);
//	/*reset op to make sure transaction terminated does not need op*/
//	if (client_transaction) {
//		belle_sip_transaction_set_application_data(BELLE_SIP_TRANSACTION(client_transaction),NULL);
//	} else {
//		belle_sip_transaction_set_application_data(BELLE_SIP_TRANSACTION(server_transaction),NULL);
//	}
	process_error(op);

}
static void process_response_event(void *op_base, const belle_sip_response_event_t *event){
	SalOp* op = (SalOp*)op_base;
	/*belle_sip_client_transaction_t *client_transaction=belle_sip_response_event_get_client_transaction(event);*/
	int code = belle_sip_response_get_status_code(belle_sip_response_event_get_response(event));
	SalTextDeliveryStatus status;
	if (code>=100 && code <200)
		status=SalTextDeliveryInProgress;
	else if (code>=200 && code <300)
		status=SalTextDeliveryDone;
	else
		status=SalTextDeliveryFailed;
	if (status != SalTextDeliveryInProgress) {
		/*reset op to make sure transaction terminated does not need op
		belle_sip_transaction_set_application_data(BELLE_SIP_TRANSACTION(client_transaction),NULL);*/
	}
	op->base.root->callbacks.text_delivery_update(op,status);

}
static bool_t is_plain_text(belle_sip_header_content_type_t* content_type) {
	return strcmp("text",belle_sip_header_content_type_get_type(content_type))==0
			&&	strcmp("plain",belle_sip_header_content_type_get_subtype(content_type))==0;
}
static bool_t is_external_body(belle_sip_header_content_type_t* content_type) {
	return strcmp("message",belle_sip_header_content_type_get_type(content_type))==0
			&&	strcmp("external-body",belle_sip_header_content_type_get_subtype(content_type))==0;
}
static bool_t is_im_iscomposing(belle_sip_header_content_type_t* content_type) {
	return strcmp("application",belle_sip_header_content_type_get_type(content_type))==0
			&&	strcmp("im-iscomposing+xml",belle_sip_header_content_type_get_subtype(content_type))==0;
}

static void process_request_event(void *op_base, const belle_sip_request_event_t *event) {
	SalOp* op = (SalOp*)op_base;
	belle_sip_request_t* req = belle_sip_request_event_get_request(event);
	belle_sip_server_transaction_t* server_transaction = belle_sip_provider_create_server_transaction(op->base.root->prov,req);
	belle_sip_header_address_t* address;
	belle_sip_header_from_t* from_header;
	belle_sip_header_content_type_t* content_type;
	belle_sip_response_t* resp;
	belle_sip_header_call_id_t* call_id = belle_sip_message_get_header_by_type(req,belle_sip_header_call_id_t);
	belle_sip_header_cseq_t* cseq = belle_sip_message_get_header_by_type(req,belle_sip_header_cseq_t);
	belle_sip_header_date_t *date=belle_sip_message_get_header_by_type(req,belle_sip_header_date_t);
	int response_code=501;
	char* from;
	bool_t plain_text=FALSE;
	bool_t external_body=FALSE;

	from_header=belle_sip_message_get_header_by_type(BELLE_SIP_MESSAGE(req),belle_sip_header_from_t);
	content_type=belle_sip_message_get_header_by_type(BELLE_SIP_MESSAGE(req),belle_sip_header_content_type_t);
	if (content_type && ((plain_text=is_plain_text(content_type))
						|| (external_body=is_external_body(content_type)))) {
		SalMessage salmsg;
		char message_id[256]={0};
		address=belle_sip_header_address_create(belle_sip_header_address_get_displayname(BELLE_SIP_HEADER_ADDRESS(from_header))
				,belle_sip_header_address_get_uri(BELLE_SIP_HEADER_ADDRESS(from_header)));
		from=belle_sip_object_to_string(BELLE_SIP_OBJECT(address));
		snprintf(message_id,sizeof(message_id)-1,"%s%i"
				,belle_sip_header_call_id_get_call_id(call_id)
				,belle_sip_header_cseq_get_seq_number(cseq));
		salmsg.from=from;
		salmsg.text=plain_text?belle_sip_message_get_body(BELLE_SIP_MESSAGE(req)):NULL;
		salmsg.url=NULL;
		if (external_body && belle_sip_parameters_get_parameter(BELLE_SIP_PARAMETERS(content_type),"URL")) {
			size_t url_length=strlen(belle_sip_parameters_get_parameter(BELLE_SIP_PARAMETERS(content_type),"URL"));
			salmsg.url = ms_strdup(belle_sip_parameters_get_parameter(BELLE_SIP_PARAMETERS(content_type),"URL")+1); /* skip first "*/
			((char*)salmsg.url)[url_length-2]='\0'; /*remove trailing "*/
		}
		salmsg.message_id=message_id;
		salmsg.time=date ? belle_sip_header_date_get_time(date) : time(NULL);
		op->base.root->callbacks.text_received(op,&salmsg);
		belle_sip_object_unref(address);
		belle_sip_free(from);
		if (salmsg.url) ms_free((char*)salmsg.url);
		response_code=200;
	} else if (content_type && is_im_iscomposing(content_type)) {
		SalIsComposing saliscomposing;
		address=belle_sip_header_address_create(belle_sip_header_address_get_displayname(BELLE_SIP_HEADER_ADDRESS(from_header))
				,belle_sip_header_address_get_uri(BELLE_SIP_HEADER_ADDRESS(from_header)));
		from=belle_sip_object_to_string(BELLE_SIP_OBJECT(address));
		saliscomposing.from=from;
		saliscomposing.text=belle_sip_message_get_body(BELLE_SIP_MESSAGE(req));
		op->base.root->callbacks.is_composing_received(op,&saliscomposing);
		belle_sip_object_unref(address);
		belle_sip_free(from);
		response_code=200;
	} else {
		ms_error("Unsupported MESSAGE with content type [%s/%s]",belle_sip_header_content_type_get_type(content_type)
				,belle_sip_header_content_type_get_subtype(content_type));
		response_code=501; /*not implemented sound appropriate*/
	}
	resp = belle_sip_response_create_from_request(req,response_code);
	belle_sip_server_transaction_send_response(server_transaction,resp);
	sal_op_release(op);
}

int sal_message_send(SalOp *op, const char *from, const char *to, const char* content_type, const char *msg){
	belle_sip_request_t* req;
	char content_type_raw[256];
	size_t content_length = msg?strlen(msg):0;
	time_t curtime=time(NULL);
	
	sal_op_message_fill_cbs(op);
	if (from)
		sal_op_set_from(op,from);
	if (to)
		sal_op_set_to(op,to);
	op->dir=SalOpDirOutgoing;

	req=sal_op_build_request(op,"MESSAGE");
	if (sal_op_get_contact(op)){
		belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),BELLE_SIP_HEADER(sal_op_create_contact(op)));
	}
	snprintf(content_type_raw,sizeof(content_type_raw),BELLE_SIP_CONTENT_TYPE ": %s",content_type);
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),BELLE_SIP_HEADER(belle_sip_header_content_type_parse(content_type_raw)));
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),BELLE_SIP_HEADER(belle_sip_header_content_length_create(content_length)));
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),BELLE_SIP_HEADER(belle_sip_header_date_create_from_time(&curtime)));
	belle_sip_message_set_body(BELLE_SIP_MESSAGE(req),msg,content_length);
	return sal_op_send_request(op,req);

}

int sal_text_send(SalOp *op, const char *from, const char *to, const char *msg) {
	return sal_message_send(op,from,to,"text/plain",msg);
}

void sal_op_message_fill_cbs(SalOp*op) {
	op->callbacks.process_io_error=process_io_error;
	op->callbacks.process_response_event=process_response_event;
	op->callbacks.process_timeout=process_timeout;
	op->callbacks.process_request_event=process_request_event;
	op->type=SalOpMessage;
}
