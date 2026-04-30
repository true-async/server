/* This is a generated file, edit HttpServerConfig.php instead. */

/* Constructor */
ZEND_BEGIN_ARG_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig___construct, 0, 0, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, host, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, port, IS_LONG, 0, "8080")
ZEND_END_ARG_INFO()

/* addListener */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_addListener, 0, 2, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tls, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

/* addUnixListener */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_addUnixListener, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* addHttp3Listener */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_addHttp3Listener, 0, 2, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* getListeners */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_getListeners, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* setBacklog */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_setBacklog, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, backlog, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* getBacklog */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_getBacklog, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* setMaxConnections */
#define arginfo_class_TrueAsync_HttpServerConfig_setMaxConnections arginfo_class_TrueAsync_HttpServerConfig_setBacklog

/* getMaxConnections */
#define arginfo_class_TrueAsync_HttpServerConfig_getMaxConnections arginfo_class_TrueAsync_HttpServerConfig_getBacklog

/* setMaxInflightRequests */
#define arginfo_class_TrueAsync_HttpServerConfig_setMaxInflightRequests arginfo_class_TrueAsync_HttpServerConfig_setBacklog

/* getMaxInflightRequests */
#define arginfo_class_TrueAsync_HttpServerConfig_getMaxInflightRequests arginfo_class_TrueAsync_HttpServerConfig_getBacklog

/* setReadTimeout */
#define arginfo_class_TrueAsync_HttpServerConfig_setReadTimeout arginfo_class_TrueAsync_HttpServerConfig_setBacklog

/* getReadTimeout */
#define arginfo_class_TrueAsync_HttpServerConfig_getReadTimeout arginfo_class_TrueAsync_HttpServerConfig_getBacklog

/* setWriteTimeout */
#define arginfo_class_TrueAsync_HttpServerConfig_setWriteTimeout arginfo_class_TrueAsync_HttpServerConfig_setBacklog

/* getWriteTimeout */
#define arginfo_class_TrueAsync_HttpServerConfig_getWriteTimeout arginfo_class_TrueAsync_HttpServerConfig_getBacklog

/* setKeepAliveTimeout */
#define arginfo_class_TrueAsync_HttpServerConfig_setKeepAliveTimeout arginfo_class_TrueAsync_HttpServerConfig_setBacklog

/* getKeepAliveTimeout */
#define arginfo_class_TrueAsync_HttpServerConfig_getKeepAliveTimeout arginfo_class_TrueAsync_HttpServerConfig_getBacklog

/* setShutdownTimeout */
#define arginfo_class_TrueAsync_HttpServerConfig_setShutdownTimeout arginfo_class_TrueAsync_HttpServerConfig_setBacklog

/* getShutdownTimeout */
#define arginfo_class_TrueAsync_HttpServerConfig_getShutdownTimeout arginfo_class_TrueAsync_HttpServerConfig_getBacklog

/* setBackpressureTargetMs */
#define arginfo_class_TrueAsync_HttpServerConfig_setBackpressureTargetMs arginfo_class_TrueAsync_HttpServerConfig_setBacklog

/* getBackpressureTargetMs */
#define arginfo_class_TrueAsync_HttpServerConfig_getBackpressureTargetMs arginfo_class_TrueAsync_HttpServerConfig_getBacklog

/* Step 8 — connection-drain knobs (all int-arg setters alias setBacklog,
 * all int-return getters alias getBacklog — same shape). */
#define arginfo_class_TrueAsync_HttpServerConfig_setMaxConnectionAgeMs       arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getMaxConnectionAgeMs       arginfo_class_TrueAsync_HttpServerConfig_getBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_setMaxConnectionAgeGraceMs  arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getMaxConnectionAgeGraceMs  arginfo_class_TrueAsync_HttpServerConfig_getBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_setDrainSpreadMs            arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getDrainSpreadMs            arginfo_class_TrueAsync_HttpServerConfig_getBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_setDrainCooldownMs          arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getDrainCooldownMs          arginfo_class_TrueAsync_HttpServerConfig_getBacklog

/* Streaming (PLAN_STREAMING) */
#define arginfo_class_TrueAsync_HttpServerConfig_setStreamWriteBufferBytes   arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getStreamWriteBufferBytes   arginfo_class_TrueAsync_HttpServerConfig_getBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_setMaxBodySize              arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getMaxBodySize              arginfo_class_TrueAsync_HttpServerConfig_getBacklog

/* WebSocket knobs (PLAN_WEBSOCKET.md §5) */
#define arginfo_class_TrueAsync_HttpServerConfig_setWsMaxMessageSize         arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getWsMaxMessageSize         arginfo_class_TrueAsync_HttpServerConfig_getBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_setWsMaxFrameSize           arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getWsMaxFrameSize           arginfo_class_TrueAsync_HttpServerConfig_getBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_setWsPingIntervalMs         arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getWsPingIntervalMs         arginfo_class_TrueAsync_HttpServerConfig_getBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_setWsPongTimeoutMs          arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getWsPongTimeoutMs          arginfo_class_TrueAsync_HttpServerConfig_getBacklog

/* HTTP/3 production knobs (NEXT_STEPS.md §5) */
#define arginfo_class_TrueAsync_HttpServerConfig_setHttp3IdleTimeoutMs        arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getHttp3IdleTimeoutMs        arginfo_class_TrueAsync_HttpServerConfig_getBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_setHttp3StreamWindowBytes    arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getHttp3StreamWindowBytes    arginfo_class_TrueAsync_HttpServerConfig_getBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_setHttp3MaxConcurrentStreams arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getHttp3MaxConcurrentStreams arginfo_class_TrueAsync_HttpServerConfig_getBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_setHttp3PeerConnectionBudget arginfo_class_TrueAsync_HttpServerConfig_setBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_getHttp3PeerConnectionBudget arginfo_class_TrueAsync_HttpServerConfig_getBacklog
#define arginfo_class_TrueAsync_HttpServerConfig_setHttp3AltSvcEnabled        arginfo_class_TrueAsync_HttpServerConfig_enableHttp2
#define arginfo_class_TrueAsync_HttpServerConfig_isHttp3AltSvcEnabled         arginfo_class_TrueAsync_HttpServerConfig_isHttp2Enabled

/* setWriteBufferSize */
#define arginfo_class_TrueAsync_HttpServerConfig_setWriteBufferSize arginfo_class_TrueAsync_HttpServerConfig_setBacklog

/* getWriteBufferSize */
#define arginfo_class_TrueAsync_HttpServerConfig_getWriteBufferSize arginfo_class_TrueAsync_HttpServerConfig_getBacklog

/* enableHttp2 */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_enableHttp2, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, enable, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* isHttp2Enabled */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_isHttp2Enabled, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* enableWebSocket */
#define arginfo_class_TrueAsync_HttpServerConfig_enableWebSocket arginfo_class_TrueAsync_HttpServerConfig_enableHttp2

/* isWebSocketEnabled */
#define arginfo_class_TrueAsync_HttpServerConfig_isWebSocketEnabled arginfo_class_TrueAsync_HttpServerConfig_isHttp2Enabled

/* enableProtocolDetection */
#define arginfo_class_TrueAsync_HttpServerConfig_enableProtocolDetection arginfo_class_TrueAsync_HttpServerConfig_enableHttp2

/* isProtocolDetectionEnabled */
#define arginfo_class_TrueAsync_HttpServerConfig_isProtocolDetectionEnabled arginfo_class_TrueAsync_HttpServerConfig_isHttp2Enabled

/* enableTls */
#define arginfo_class_TrueAsync_HttpServerConfig_enableTls arginfo_class_TrueAsync_HttpServerConfig_enableHttp2

/* isTlsEnabled */
#define arginfo_class_TrueAsync_HttpServerConfig_isTlsEnabled arginfo_class_TrueAsync_HttpServerConfig_isHttp2Enabled

/* setCertificate */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_setCertificate, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* getCertificate */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_getCertificate, 0, 0, IS_STRING, 1)
ZEND_END_ARG_INFO()

/* setPrivateKey */
#define arginfo_class_TrueAsync_HttpServerConfig_setPrivateKey arginfo_class_TrueAsync_HttpServerConfig_setCertificate

/* getPrivateKey */
#define arginfo_class_TrueAsync_HttpServerConfig_getPrivateKey arginfo_class_TrueAsync_HttpServerConfig_getCertificate

/* setAutoAwaitBody */
#define arginfo_class_TrueAsync_HttpServerConfig_setAutoAwaitBody arginfo_class_TrueAsync_HttpServerConfig_enableHttp2

/* isAutoAwaitBodyEnabled */
#define arginfo_class_TrueAsync_HttpServerConfig_isAutoAwaitBodyEnabled arginfo_class_TrueAsync_HttpServerConfig_isHttp2Enabled

/* isLocked */
#define arginfo_class_TrueAsync_HttpServerConfig_isLocked arginfo_class_TrueAsync_HttpServerConfig_isHttp2Enabled

/* Logging / telemetry — Step 1 PLAN_LOG.md */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_setLogSeverity, 0, 1, IS_STATIC, 0)
	ZEND_ARG_OBJ_INFO(0, level, TrueAsync\\LogSeverity, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_getLogSeverity, 0, 0, TrueAsync\\LogSeverity, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_setLogStream, 0, 1, IS_STATIC, 0)
	ZEND_ARG_TYPE_INFO(0, stream, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_TrueAsync_HttpServerConfig_getLogStream, 0, 0, IS_MIXED, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_TrueAsync_HttpServerConfig_setTelemetryEnabled arginfo_class_TrueAsync_HttpServerConfig_enableHttp2
#define arginfo_class_TrueAsync_HttpServerConfig_isTelemetryEnabled  arginfo_class_TrueAsync_HttpServerConfig_isHttp2Enabled

/* Method declarations */
ZEND_METHOD(TrueAsync_HttpServerConfig, __construct);
ZEND_METHOD(TrueAsync_HttpServerConfig, addListener);
ZEND_METHOD(TrueAsync_HttpServerConfig, addUnixListener);
ZEND_METHOD(TrueAsync_HttpServerConfig, addHttp3Listener);
ZEND_METHOD(TrueAsync_HttpServerConfig, getListeners);
ZEND_METHOD(TrueAsync_HttpServerConfig, setBacklog);
ZEND_METHOD(TrueAsync_HttpServerConfig, getBacklog);
ZEND_METHOD(TrueAsync_HttpServerConfig, setMaxConnections);
ZEND_METHOD(TrueAsync_HttpServerConfig, getMaxConnections);
ZEND_METHOD(TrueAsync_HttpServerConfig, setMaxInflightRequests);
ZEND_METHOD(TrueAsync_HttpServerConfig, getMaxInflightRequests);
ZEND_METHOD(TrueAsync_HttpServerConfig, setReadTimeout);
ZEND_METHOD(TrueAsync_HttpServerConfig, getReadTimeout);
ZEND_METHOD(TrueAsync_HttpServerConfig, setWriteTimeout);
ZEND_METHOD(TrueAsync_HttpServerConfig, getWriteTimeout);
ZEND_METHOD(TrueAsync_HttpServerConfig, setKeepAliveTimeout);
ZEND_METHOD(TrueAsync_HttpServerConfig, getKeepAliveTimeout);
ZEND_METHOD(TrueAsync_HttpServerConfig, setShutdownTimeout);
ZEND_METHOD(TrueAsync_HttpServerConfig, getShutdownTimeout);
ZEND_METHOD(TrueAsync_HttpServerConfig, setBackpressureTargetMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, getBackpressureTargetMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, setMaxConnectionAgeMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, getMaxConnectionAgeMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, setMaxConnectionAgeGraceMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, getMaxConnectionAgeGraceMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, setDrainSpreadMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, getDrainSpreadMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, setDrainCooldownMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, getDrainCooldownMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, setStreamWriteBufferBytes);
ZEND_METHOD(TrueAsync_HttpServerConfig, getStreamWriteBufferBytes);
ZEND_METHOD(TrueAsync_HttpServerConfig, setMaxBodySize);
ZEND_METHOD(TrueAsync_HttpServerConfig, getMaxBodySize);
ZEND_METHOD(TrueAsync_HttpServerConfig, setWsMaxMessageSize);
ZEND_METHOD(TrueAsync_HttpServerConfig, getWsMaxMessageSize);
ZEND_METHOD(TrueAsync_HttpServerConfig, setWsMaxFrameSize);
ZEND_METHOD(TrueAsync_HttpServerConfig, getWsMaxFrameSize);
ZEND_METHOD(TrueAsync_HttpServerConfig, setWsPingIntervalMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, getWsPingIntervalMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, setWsPongTimeoutMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, getWsPongTimeoutMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3IdleTimeoutMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, getHttp3IdleTimeoutMs);
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3StreamWindowBytes);
ZEND_METHOD(TrueAsync_HttpServerConfig, getHttp3StreamWindowBytes);
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3MaxConcurrentStreams);
ZEND_METHOD(TrueAsync_HttpServerConfig, getHttp3MaxConcurrentStreams);
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3PeerConnectionBudget);
ZEND_METHOD(TrueAsync_HttpServerConfig, getHttp3PeerConnectionBudget);
ZEND_METHOD(TrueAsync_HttpServerConfig, setHttp3AltSvcEnabled);
ZEND_METHOD(TrueAsync_HttpServerConfig, isHttp3AltSvcEnabled);
ZEND_METHOD(TrueAsync_HttpServerConfig, setWriteBufferSize);
ZEND_METHOD(TrueAsync_HttpServerConfig, getWriteBufferSize);
ZEND_METHOD(TrueAsync_HttpServerConfig, enableHttp2);
ZEND_METHOD(TrueAsync_HttpServerConfig, isHttp2Enabled);
ZEND_METHOD(TrueAsync_HttpServerConfig, enableWebSocket);
ZEND_METHOD(TrueAsync_HttpServerConfig, isWebSocketEnabled);
ZEND_METHOD(TrueAsync_HttpServerConfig, enableProtocolDetection);
ZEND_METHOD(TrueAsync_HttpServerConfig, isProtocolDetectionEnabled);
ZEND_METHOD(TrueAsync_HttpServerConfig, enableTls);
ZEND_METHOD(TrueAsync_HttpServerConfig, isTlsEnabled);
ZEND_METHOD(TrueAsync_HttpServerConfig, setCertificate);
ZEND_METHOD(TrueAsync_HttpServerConfig, getCertificate);
ZEND_METHOD(TrueAsync_HttpServerConfig, setPrivateKey);
ZEND_METHOD(TrueAsync_HttpServerConfig, getPrivateKey);
ZEND_METHOD(TrueAsync_HttpServerConfig, setAutoAwaitBody);
ZEND_METHOD(TrueAsync_HttpServerConfig, isAutoAwaitBodyEnabled);
ZEND_METHOD(TrueAsync_HttpServerConfig, isLocked);
ZEND_METHOD(TrueAsync_HttpServerConfig, setLogSeverity);
ZEND_METHOD(TrueAsync_HttpServerConfig, getLogSeverity);
ZEND_METHOD(TrueAsync_HttpServerConfig, setLogStream);
ZEND_METHOD(TrueAsync_HttpServerConfig, getLogStream);
ZEND_METHOD(TrueAsync_HttpServerConfig, setTelemetryEnabled);
ZEND_METHOD(TrueAsync_HttpServerConfig, isTelemetryEnabled);

/* Method table */
static const zend_function_entry class_TrueAsync_HttpServerConfig_methods[] = {
	ZEND_ME(TrueAsync_HttpServerConfig, __construct, arginfo_class_TrueAsync_HttpServerConfig___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, addListener, arginfo_class_TrueAsync_HttpServerConfig_addListener, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, addUnixListener, arginfo_class_TrueAsync_HttpServerConfig_addUnixListener, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, addHttp3Listener, arginfo_class_TrueAsync_HttpServerConfig_addHttp3Listener, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getListeners, arginfo_class_TrueAsync_HttpServerConfig_getListeners, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setBacklog, arginfo_class_TrueAsync_HttpServerConfig_setBacklog, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getBacklog, arginfo_class_TrueAsync_HttpServerConfig_getBacklog, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setMaxConnections, arginfo_class_TrueAsync_HttpServerConfig_setMaxConnections, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getMaxConnections, arginfo_class_TrueAsync_HttpServerConfig_getMaxConnections, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setMaxInflightRequests, arginfo_class_TrueAsync_HttpServerConfig_setMaxInflightRequests, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getMaxInflightRequests, arginfo_class_TrueAsync_HttpServerConfig_getMaxInflightRequests, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setReadTimeout, arginfo_class_TrueAsync_HttpServerConfig_setReadTimeout, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getReadTimeout, arginfo_class_TrueAsync_HttpServerConfig_getReadTimeout, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setWriteTimeout, arginfo_class_TrueAsync_HttpServerConfig_setWriteTimeout, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getWriteTimeout, arginfo_class_TrueAsync_HttpServerConfig_getWriteTimeout, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setKeepAliveTimeout, arginfo_class_TrueAsync_HttpServerConfig_setKeepAliveTimeout, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getKeepAliveTimeout, arginfo_class_TrueAsync_HttpServerConfig_getKeepAliveTimeout, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setShutdownTimeout, arginfo_class_TrueAsync_HttpServerConfig_setShutdownTimeout, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getShutdownTimeout, arginfo_class_TrueAsync_HttpServerConfig_getShutdownTimeout, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setBackpressureTargetMs, arginfo_class_TrueAsync_HttpServerConfig_setBackpressureTargetMs, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getBackpressureTargetMs, arginfo_class_TrueAsync_HttpServerConfig_getBackpressureTargetMs, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setMaxConnectionAgeMs,       arginfo_class_TrueAsync_HttpServerConfig_setMaxConnectionAgeMs,       ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getMaxConnectionAgeMs,       arginfo_class_TrueAsync_HttpServerConfig_getMaxConnectionAgeMs,       ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setMaxConnectionAgeGraceMs,  arginfo_class_TrueAsync_HttpServerConfig_setMaxConnectionAgeGraceMs,  ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getMaxConnectionAgeGraceMs,  arginfo_class_TrueAsync_HttpServerConfig_getMaxConnectionAgeGraceMs,  ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setDrainSpreadMs,            arginfo_class_TrueAsync_HttpServerConfig_setDrainSpreadMs,            ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getDrainSpreadMs,            arginfo_class_TrueAsync_HttpServerConfig_getDrainSpreadMs,            ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setDrainCooldownMs,          arginfo_class_TrueAsync_HttpServerConfig_setDrainCooldownMs,          ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getDrainCooldownMs,          arginfo_class_TrueAsync_HttpServerConfig_getDrainCooldownMs,          ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setStreamWriteBufferBytes,   arginfo_class_TrueAsync_HttpServerConfig_setStreamWriteBufferBytes,   ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getStreamWriteBufferBytes,   arginfo_class_TrueAsync_HttpServerConfig_getStreamWriteBufferBytes,   ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setMaxBodySize,              arginfo_class_TrueAsync_HttpServerConfig_setMaxBodySize,              ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getMaxBodySize,              arginfo_class_TrueAsync_HttpServerConfig_getMaxBodySize,              ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setWsMaxMessageSize,         arginfo_class_TrueAsync_HttpServerConfig_setWsMaxMessageSize,         ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getWsMaxMessageSize,         arginfo_class_TrueAsync_HttpServerConfig_getWsMaxMessageSize,         ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setWsMaxFrameSize,           arginfo_class_TrueAsync_HttpServerConfig_setWsMaxFrameSize,           ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getWsMaxFrameSize,           arginfo_class_TrueAsync_HttpServerConfig_getWsMaxFrameSize,           ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setWsPingIntervalMs,         arginfo_class_TrueAsync_HttpServerConfig_setWsPingIntervalMs,         ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getWsPingIntervalMs,         arginfo_class_TrueAsync_HttpServerConfig_getWsPingIntervalMs,         ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setWsPongTimeoutMs,          arginfo_class_TrueAsync_HttpServerConfig_setWsPongTimeoutMs,          ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getWsPongTimeoutMs,          arginfo_class_TrueAsync_HttpServerConfig_getWsPongTimeoutMs,          ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setHttp3IdleTimeoutMs,        arginfo_class_TrueAsync_HttpServerConfig_setHttp3IdleTimeoutMs,        ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getHttp3IdleTimeoutMs,        arginfo_class_TrueAsync_HttpServerConfig_getHttp3IdleTimeoutMs,        ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setHttp3StreamWindowBytes,    arginfo_class_TrueAsync_HttpServerConfig_setHttp3StreamWindowBytes,    ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getHttp3StreamWindowBytes,    arginfo_class_TrueAsync_HttpServerConfig_getHttp3StreamWindowBytes,    ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setHttp3MaxConcurrentStreams, arginfo_class_TrueAsync_HttpServerConfig_setHttp3MaxConcurrentStreams, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getHttp3MaxConcurrentStreams, arginfo_class_TrueAsync_HttpServerConfig_getHttp3MaxConcurrentStreams, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setHttp3PeerConnectionBudget, arginfo_class_TrueAsync_HttpServerConfig_setHttp3PeerConnectionBudget, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getHttp3PeerConnectionBudget, arginfo_class_TrueAsync_HttpServerConfig_getHttp3PeerConnectionBudget, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setHttp3AltSvcEnabled,        arginfo_class_TrueAsync_HttpServerConfig_setHttp3AltSvcEnabled,        ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, isHttp3AltSvcEnabled,         arginfo_class_TrueAsync_HttpServerConfig_isHttp3AltSvcEnabled,         ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setWriteBufferSize, arginfo_class_TrueAsync_HttpServerConfig_setWriteBufferSize, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getWriteBufferSize, arginfo_class_TrueAsync_HttpServerConfig_getWriteBufferSize, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, enableHttp2, arginfo_class_TrueAsync_HttpServerConfig_enableHttp2, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, isHttp2Enabled, arginfo_class_TrueAsync_HttpServerConfig_isHttp2Enabled, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, enableWebSocket, arginfo_class_TrueAsync_HttpServerConfig_enableWebSocket, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, isWebSocketEnabled, arginfo_class_TrueAsync_HttpServerConfig_isWebSocketEnabled, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, enableProtocolDetection, arginfo_class_TrueAsync_HttpServerConfig_enableProtocolDetection, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, isProtocolDetectionEnabled, arginfo_class_TrueAsync_HttpServerConfig_isProtocolDetectionEnabled, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, enableTls, arginfo_class_TrueAsync_HttpServerConfig_enableTls, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, isTlsEnabled, arginfo_class_TrueAsync_HttpServerConfig_isTlsEnabled, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setCertificate, arginfo_class_TrueAsync_HttpServerConfig_setCertificate, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getCertificate, arginfo_class_TrueAsync_HttpServerConfig_getCertificate, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setPrivateKey, arginfo_class_TrueAsync_HttpServerConfig_setPrivateKey, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getPrivateKey, arginfo_class_TrueAsync_HttpServerConfig_getPrivateKey, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setAutoAwaitBody, arginfo_class_TrueAsync_HttpServerConfig_setAutoAwaitBody, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, isAutoAwaitBodyEnabled, arginfo_class_TrueAsync_HttpServerConfig_isAutoAwaitBodyEnabled, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, isLocked, arginfo_class_TrueAsync_HttpServerConfig_isLocked, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setLogSeverity,      arginfo_class_TrueAsync_HttpServerConfig_setLogSeverity,      ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getLogSeverity,      arginfo_class_TrueAsync_HttpServerConfig_getLogSeverity,      ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setLogStream,        arginfo_class_TrueAsync_HttpServerConfig_setLogStream,        ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, getLogStream,        arginfo_class_TrueAsync_HttpServerConfig_getLogStream,        ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, setTelemetryEnabled, arginfo_class_TrueAsync_HttpServerConfig_setTelemetryEnabled, ZEND_ACC_PUBLIC)
	ZEND_ME(TrueAsync_HttpServerConfig, isTelemetryEnabled,  arginfo_class_TrueAsync_HttpServerConfig_isTelemetryEnabled,  ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

/* Class registration */
static zend_class_entry *register_class_TrueAsync_HttpServerConfig(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "TrueAsync", "HttpServerConfig", class_TrueAsync_HttpServerConfig_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES);

	return class_entry;
}
