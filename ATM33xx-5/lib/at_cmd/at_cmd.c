/**
 *******************************************************************************
 *
 * @file at_cmd.c
 *
 * @brief Atmosic AT Command Core
 *
 * Copyright (C) Atmosic 2021-2024
 *
 *******************************************************************************
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include "at_cmd.h"
#ifndef CONFIG_SOC_FAMILY_ATM
#include "atm_log.h"
ATM_LOG_LOCAL_SETTING("at_cmd", N);
#else
#include <assert.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(at_cmd, LOG_LEVEL_INF);
#undef ATM_LOG
#define ATM_LOG(MSK, fmt, ...) LOG_INF(fmt, ##__VA_ARGS__)
#define ASSERT_ERR(arg) __ASSERT(arg, "%s", __func__)
#define ATM_LOG_FCYAN
#define ATM_LOG_FRED
#define ATM_LOG_FGREEN
#define ATM_LOG_DECOLOR
#define D_(d) #d
#define STR(d) D_(d)
#endif
#include "atm_utils_math.h"

#ifndef AT_CMD_PREFIX_CTX
#define AT_CMD_PREFIX "\r\n"
#else // AT_CMD_PREFIX_CTX
#define AT_CMD_PREFIX STR(AT_CMD_PREFIX_CTX)
#endif // AT_CMD_PREFIX_CTX

#ifndef AT_CMD_POSTFIX_CTX
#define AT_CMD_POSTFIX "\r\n"
#else // AT_CMD_POSTFIX_CTX
#define AT_CMD_POSTFIX STR(AT_CMD_POSTFIX_CTX)
#endif // AT_CMD_POSTFIX_CTX

#ifndef AT_CMD_RESP_OK
#define AT_CMD_RESP_OK "OK"
#endif // AT_CMD_RESP_OK

#ifndef AT_CMD_RESP_ERR
#define AT_CMD_RESP_ERR "ERR"
#endif // AT_CMD_RESP_ERR

#ifndef AT_CMD_HDR
#define AT_CMD_HDR "AT+"
#endif // AT_CMD_HDR

#define AT_CMD_HDR_LEN (sizeof(AT_CMD_HDR) - 1)

#ifdef __ARMCC_VERSION
#define __at_cmd_start Load$$at_cmd$$Base
#define __at_cmd_end Load$$at_cmd$$Limit
#elif defined(__ICCARM__)
#define __at_cmd_start at_cmd$$Base
#define __at_cmd_end at_cmd$$Limit
#endif

extern at_cmd_t const __at_cmd_start;
extern at_cmd_t const __at_cmd_end;
#define AT_CMD_START (&__at_cmd_start)
#define AT_CMD_END (&__at_cmd_end)

static bool at_cmd_dbg;

static at_cmd_alloc_ctx_t at_ctx[AT_CMD_XFER_MAX_NUM];

static void at_cmd_resp_data(at_cmd_ch_t ch, char const *data, uint16_t len)
{
    if (at_ctx[ch].resp) {
	at_ctx[ch].resp(ch, data, len);
    }
}

static void
at_send_status(at_cmd_ch_t ch, at_cmd_err_t err, at_cmd_t const *cmd, ...)
{
    if (err == AT_CMD_ERR_NO_ERROR) {
	at_cmd_resp_concat(ch, at_all, "%s", AT_CMD_RESP_OK);
    } else if (err == AT_CMD_ERR_SPECIFIC_ERR) {
	va_list args;
	va_start(args, cmd);
	uint16_t app_err = va_arg(args, int);
	va_end(args);
	at_cmd_resp_concat(ch, at_all, "+%s:%X", cmd->str, app_err);
	at_cmd_resp_concat(ch, at_all, "%s", AT_CMD_RESP_ERR);
    } else {
	at_cmd_resp_concat(ch, at_all, "%s:%d", AT_CMD_RESP_ERR, err);
    }
}

static void at_cmd_check_args(at_cmd_param_t *param)
{
    param->err = AT_CMD_ERR_NO_ERROR;
    param->args = at_pasr_param();
    uint16_t num = at_pasr_param_num_get();

    ATM_LOG(V, "%s: param->argc (%d), num (%d)", __func__, param->argc, num);

    if (num != param->argc) {
	// Apply correct parameter number to parser
	param->argc = num;
	param->err = AT_CMD_ERR_WRONG_ARGU_CNT;
	return;
    }

    for (int i = 0; i < num; i++) {
	if (param->args[i]->status != AT_PASR_OK &&
	    param->args[i]->status != AT_PASR_EMPTY_DATA) {
	    param->err = AT_CMD_ERR_WRONG_ARGU_TYPE_OR_RANGE;
	    ATM_LOG(V, "%s: [%d] status (%d)", __func__, i,
		param->args[i]->status);
	    break;
	}
    }
}

static char const * const at_cmd_dt_str[] = {
    ATM_LOG_FCYAN STR(at_pasr_dt_i8) ATM_LOG_DECOLOR,
    ATM_LOG_FCYAN STR(at_pasr_dt_u8) ATM_LOG_DECOLOR,
    ATM_LOG_FCYAN STR(at_pasr_dt_i16) ATM_LOG_DECOLOR,
    ATM_LOG_FCYAN STR(at_pasr_dt_u16) ATM_LOG_DECOLOR,
    ATM_LOG_FCYAN STR(at_pasr_dt_i32) ATM_LOG_DECOLOR,
    ATM_LOG_FCYAN STR(at_pasr_dt_u32) ATM_LOG_DECOLOR,
    ATM_LOG_FCYAN STR(at_pasr_dt_array) ATM_LOG_DECOLOR,
    ATM_LOG_FCYAN STR(at_pasr_dt_string) ATM_LOG_DECOLOR,
    ATM_LOG_FRED STR(at_pasr_dt_unknow) ATM_LOG_DECOLOR,
};

static char const * const at_cmd_err_str[] = {
    ATM_LOG_FGREEN STR(AT_PASR_OK) ATM_LOG_DECOLOR,
    ATM_LOG_FRED STR(AT_PASR_RANGE_EXCEED) ATM_LOG_DECOLOR,
    ATM_LOG_FRED STR(AT_PASR_WRONG_TYPE) ATM_LOG_DECOLOR,
    ATM_LOG_FRED STR(AT_PASR_BUSY) ATM_LOG_DECOLOR,
    ATM_LOG_FRED STR(AT_PASR_EMPTY_DATA) ATM_LOG_DECOLOR,
    ATM_LOG_FRED STR(AT_PASR_NO_MEMORY) ATM_LOG_DECOLOR,
    ATM_LOG_FRED STR(AT_PASR_INVALID_DATA) ATM_LOG_DECOLOR,
    ATM_LOG_FRED STR(AT_PASR_MORE_DATA) ATM_LOG_DECOLOR
};

static void at_cmd_dbg_arg(at_pasr_tlv_t const *tlv)
{
    switch (tlv->type) {
	case at_pasr_dt_i16: {
	    ATM_LOG(N, "%s : %d (%s)[%d~%d]", at_cmd_dt_str[tlv->type],
		tlv->i16, at_cmd_err_str[tlv->status], tlv->i16min, tlv->i16max);
	} break;

	case at_pasr_dt_u16: {
	    ATM_LOG(N, "%s : %u (%s)[%u~%u]", at_cmd_dt_str[tlv->type],
		tlv->u16, at_cmd_err_str[tlv->status], tlv->u16min, tlv->u16max);
	} break;

	case at_pasr_dt_u8: {
	    ATM_LOG(N, "%s : %u (%s)[%u~%u]", at_cmd_dt_str[tlv->type],
		tlv->u8, at_cmd_err_str[tlv->status], tlv->u8min, tlv->u8max);
	} break;

	case at_pasr_dt_i8: {
	    ATM_LOG(N, "%s : %d (%s)[%i~%i]", at_cmd_dt_str[tlv->type],
		tlv->i8, at_cmd_err_str[tlv->status], tlv->i8min, tlv->i8max);
	} break;

	case at_pasr_dt_array: {
	    ATM_LOG(N, "%s : 0x%02X...0x%02X (%s)", at_cmd_dt_str[tlv->type],
		tlv->array[0], tlv->array[tlv->length - 1],
		at_cmd_err_str[tlv->status]);
	} break;

	case at_pasr_dt_i32: {
	    ATM_LOG(N, "%s : %" PRId32 " (%s)[%" PRId32 "~%" PRId32 "]",
		at_cmd_dt_str[tlv->type], tlv->i32,
		at_cmd_err_str[tlv->status], tlv->i32min, tlv->i32max);
	} break;

	case at_pasr_dt_u32: {
	    ATM_LOG(N, "%s : %" PRIu32 " (%s)[%" PRIu32 "~%" PRIu32 "]",
		at_cmd_dt_str[tlv->type], tlv->u32,
		at_cmd_err_str[tlv->status], tlv->u32min, tlv->u32max);
	} break;

	case at_pasr_dt_string: {
	    ATM_LOG(N, "%s : %.*s (%s)", at_cmd_dt_str[tlv->type], tlv->length,
		tlv->string, at_cmd_err_str[tlv->status]);
	} break;

	case at_pasr_dt_unknow: {
	    ATM_LOG(N, "%s : ", at_cmd_dt_str[tlv->type]);
	} break;

	default:
	    break;
    }
}

static void at_call_handler(at_cmd_ch_t ch, at_cmd_type_t type,
    at_cmd_t const *cmd, bool cmd_residue)
{
    at_cmd_param_t param = {
	.type = type,
	.argc = cmd->param_num,
	.ch = ch,
	.str = cmd->str,
    };

    if (type == at_cmd_type_exec) {
	at_cmd_check_args(&param);

	if (at_cmd_dbg) {
	    for (int i = 0; i < param.argc; i++) {
		at_cmd_dbg_arg(param.args[i]);
	    }
	}

	if (cmd_residue) {
	    param.err = AT_CMD_ERR_WRONG_ARGU_CNT;
	}
	cmd->hdlr(&param);
	at_send_status(param.ch, param.err, cmd, param.app_err);
    } else {
	param.err = AT_CMD_ERR_NO_ERROR;
	cmd->hdlr(&param);
    }
}

static at_cmd_t const *at_cmd_enum(at_cmd_t const *curr)
{
    if (!curr) {
	return AT_CMD_START;
    }
    curr++;

    if (curr == AT_CMD_END) {
	return NULL;
    }

    return curr;
}

static char const *at_cmd_xfer_uart(at_cmd_ch_t ch, void const *data,
    uint16_t *len)
{
    uint16_t xlen = *len;
    if (xlen <= AT_CMD_HDR_LEN) {
	if ((xlen == AT_CMD_HDR_LEN - 1) &&
	    !strncmp(data, AT_CMD_HDR, AT_CMD_HDR_LEN - 1)) {
	    *len = 0;
	    return data;
	}
	return NULL;
    }

    *len -= AT_CMD_HDR_LEN;
    return ((char const *)data + AT_CMD_HDR_LEN);
}

static at_cmd_t const *at_cmd_content_get(uint16_t idx)
{
    if (idx >= at_cmd_count()) {
	return NULL;
    }

    return (AT_CMD_START + idx);
}

static at_cmd_t const *at_cmd_is_available(char const *cmd_str)
{
    if (!*cmd_str) {
	return NULL;
    }

    char const *em = strchr(cmd_str, '=');
    char const *qm = strchr(cmd_str, '?');
    size_t cmd_str_len;
    if (!em && !qm) {
	cmd_str_len = strlen(cmd_str);
    } else {
	char const *lm = (qm ? (em ? (qm > em ? em : qm) : qm) : em);
	cmd_str_len = lm - cmd_str;
    }

    if (!cmd_str_len) {
	return NULL;
    }

    uint16_t bottom = at_cmd_count();
    for (uint16_t upper = 0; upper < bottom; ) {
	uint16_t middle = (upper + bottom) / 2;
	at_cmd_t const *tcmd = at_cmd_content_get(middle);
	if (!tcmd) {
	    ATM_LOG(E, "%s: Could not get AT command content!", __func__);
	    ASSERT_ERR(tcmd);
	    return false;
	}

	size_t tlen = strlen(tcmd->str);
	int result = strncmp(cmd_str, tcmd->str, ATM_MIN(cmd_str_len, tlen));

	if (!result && (cmd_str_len == tlen)) {
	    return tcmd;
	} else if (result ? (result > 0) : (cmd_str_len > tlen)) {
	    upper = middle + 1;
	} else {
	    bottom = middle;
	}
    }

    return NULL;
}

bool at_cmd_proc(at_cmd_ch_t ch, void const *data, uint16_t data_len)
{
    if (!data_len) {
	return false;
    }

    char const *astr = data;
    uint16_t xlen = data_len;
    if (at_ctx[ch].xfer) {
	astr = at_ctx[ch].xfer(ch, data, &xlen);
	if (!astr) {
	    at_send_status(ch, AT_CMD_ERR_NOT_SUPPORT, NULL);
	    return false;
	}

	if (!xlen) {
	    at_send_status(ch, AT_CMD_ERR_NO_ERROR, NULL);
	    return true;
	}
    }

    ATM_LOG(V, "CMD (%s)", astr);
    at_cmd_t const *cmd = at_cmd_is_available(astr);
    if (!cmd) {
	at_send_status(ch, AT_CMD_ERR_NOT_SUPPORT, NULL);
	return false;
    }

    size_t clen = strlen(cmd->str);
    if (xlen == clen) {
	at_send_status(ch, AT_CMD_ERR_NOT_SUPPORT, cmd);
	return false;
    }

    if ((xlen == clen + 1) && (astr[clen] == '?')) {
	at_call_handler(ch, at_cmd_type_query, cmd, false);
	at_send_status(ch, AT_CMD_ERR_NO_ERROR, cmd);
	return true;
    }

    if ((xlen == clen + 2) && (astr[clen] == '=') && (astr[clen + 1] == '?')) {
	at_cmd_resp_concat(ch, at_all, "+%s:%s", cmd->str, cmd->test_str);
	at_send_status(ch, AT_CMD_ERR_NO_ERROR, cmd);
	return true;
    }

    bool residue = (at_pasr_param_validate(astr + clen + 1, cmd->fmt) ==
	AT_PASR_MORE_DATA);
    at_call_handler(ch, at_cmd_type_exec, cmd, residue);
    at_pasr_clear();
    return true;
}

at_cmd_ch_t at_cmd_alloc(at_cmd_alloc_ctx_t const *ctx)
{
    for (int ch = 0; ch < AT_CMD_XFER_MAX_NUM; ch++) {
	if (!at_ctx[ch].resp) {
	    at_ctx[ch].xfer = (ctx->xfer != AT_CMD_DFT_XFER_UART) ?
		ctx->xfer : at_cmd_xfer_uart;
	    at_ctx[ch].resp = ctx->resp;
	    return ch;
	}
    }

    ATM_LOG(E, "Please increase the AT_CMD_XFER_MAX_NUM in makefile to support "
	"more transfer layer!");
    ASSERT_ERR(0);
    return AT_CMD_INVALID_CH;
}

static int at_cmd_prefix(char *buf, int offset, at_cmd_resp_flag_t flag)
{
    if (flag & at_prefix) {
	strcpy(&buf[offset], AT_CMD_PREFIX);
	offset += strlen(AT_CMD_PREFIX);
    }
    return offset;
}

static int at_cmd_postfix(char *buf, int offset, at_cmd_resp_flag_t flag)
{
    if (flag & at_postfix) {
	uint8_t plen = strlen(AT_CMD_POSTFIX);
	if ((offset + plen) <= AT_CMD_RESP_LEN) {
	    strcpy(&buf[offset], AT_CMD_POSTFIX);
	}
	offset += plen;
    }

    if (offset > AT_CMD_RESP_LEN) {
	ATM_LOG(E, "Please increase AT_CMD_RESP_LEN in compile option!");
	offset = AT_CMD_RESP_LEN;
    }

    return offset;
}

void at_cmd_resp_array(at_cmd_ch_t ch, at_cmd_resp_flag_t flag, uint16_t len,
    uint8_t const *dat)
{
    char buffer[AT_CMD_RESP_LEN + 1];
    int offset = 0;

    offset = at_cmd_prefix(buffer, offset, flag);

    for (uint16_t i = 0; i < len; i++) {
	int elen = AT_CMD_RESP_LEN + 1 - offset;
	int rlen = snprintf(&buffer[offset], elen, "%02X", dat[i]);
	offset += rlen;
    }

    offset = at_cmd_postfix(buffer, offset, flag);

    at_cmd_resp_data(ch, buffer, offset);
}

void at_cmd_resp_concat(at_cmd_ch_t ch, at_cmd_resp_flag_t flag,
    char const *fmt, ...)
{
    char buffer[AT_CMD_RESP_LEN + 1];
    int offset = 0;

    offset = at_cmd_prefix(buffer, offset, flag);

    if (flag & at_data) {
	va_list args;
	va_start(args, fmt);
	int elen = AT_CMD_RESP_LEN + 1 - offset;
	int rlen = vsnprintf(&buffer[offset], elen, fmt, args);
	va_end(args);

	offset += rlen;
    }

    offset = at_cmd_postfix(buffer, offset, flag);

    at_cmd_resp_data(ch, buffer, offset);
}

void at_cmd_resp_raw(at_cmd_ch_t ch, void const *data, uint16_t len)
{
#ifndef AUTO_TEST
    if (at_ctx[ch].resp) {
	at_ctx[ch].resp(ch, data, len);
    }
#endif
}

uint16_t at_cmd_count(void)
{
    return (AT_CMD_END - AT_CMD_START);
}

/*
 * AT COMMAND HANDLER (built-in command)
 *******************************************************************************
 */
#ifndef NO_AT_CMD_BUILTIN
#define CMD_NAME_LIST "LISTCMDS"
#define CMD_PARM_FMT_LIST ""
#define CMD_PARM_NUM_LIST 0
#define CMD_PARM_DESC_LIST "<List all AT commands>"

static void at_cmd_list_print(at_cmd_ch_t ch)
{
    ATM_LOG(V, "%s: %d AT commands available", __func__, at_cmd_count());
    for (at_cmd_t const *next = at_cmd_enum(NULL); next;
	next = at_cmd_enum(next)) {
	at_cmd_resp_concat(ch, at_prefix | at_data, "+%s:%s", next->str,
	    next->test_str);
    }
}

static void at_cmd_list_hdlr(at_cmd_param_t *param)
{
    if ((param->type == at_cmd_type_query) && !param->err) {
	at_cmd_list_print(param->ch);
    }
}

AT_COMMAND(CMD_NAME_LIST, CMD_PARM_FMT_LIST, CMD_PARM_NUM_LIST, at_cmd_list_hdlr,
    CMD_PARM_DESC_LIST);

#define CMD_NAME_DBG "DEBUG"
#define CMD_PARM_FMT_DBG "B(0~1)"
#define CMD_PARM_NUM_DBG 1
#define CMD_PARM_DESC_DBG "<1 or 0>"

static void at_cmd_dbg_hdlr(at_cmd_param_t *param)
{
    if ((param->type == at_cmd_type_exec) && !param->err) {
	at_cmd_dbg = AT_PASR_GET_PARAM(param, u8, 0);
    }
}

AT_COMMAND(CMD_NAME_DBG, CMD_PARM_FMT_DBG, CMD_PARM_NUM_DBG, at_cmd_dbg_hdlr,
    CMD_PARM_DESC_DBG);
#endif // NO_AT_CMD_BUILTIN

