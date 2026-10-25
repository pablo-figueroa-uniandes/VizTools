// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0 OR MIT

#pragma once

#include <string.h>
#include <stdint.h>


/*
 * Follows the form of OpenXR call ABI macros.
 *
 * Function declaration:
 *     CXRAPI_ATTR void CXRAPI_CALL
 *     nv_cxr_function(void);
 *
 * Function pointer type:
 *     typedef void (CXRAPI_PTR *PFN_nv_cxr_function)(void);
 */
#if defined(__linux__)
#define CXRAPI_ATTR
#define CXRAPI_CALL
#define CXRAPI_PTR
#elif defined(_WIN32)
#define CXRAPI_ATTR
#define CXRAPI_CALL __stdcall
#define CXRAPI_PTR CXRAPI_CALL
#else
#error "Platform not supported."
#endif


#ifdef __cplusplus
extern "C" {
#endif

#define NV_CXR_API_VERSION_MAJOR 1
#define NV_CXR_API_VERSION_MINOR 0
#define NV_CXR_API_VERSION_PATCH 7


/*!
 * We use a C API to have a very strict layer of separation to users of this
 * library, internal as well as potential external ones. Having this layer here
 * means that we can do a drop in replacement of the runtime with the *exact*
 * same release of KitXR (or any other user) meaning we have much less process
 * to go through when we want to update just the runtime.
 */

/*!
 * Returns the version of the API, useful for langagues that dynamically loads
 * the library to know that the thing they loaded is compatible. This is the
 * version that gates which functions are available to be called at all.
 */
CXRAPI_ATTR void CXRAPI_CALL
nv_cxr_get_library_api_version(uint32_t *major, uint32_t *minor, uint32_t *patch);

/*!
 * Returns the version of the runtime itself, this is useful for printing into
 * logs to assist debugging.
 *
 * The major number is also the protocol version, so it can be shown in the app
 * UI to give the user information of which clients are supported.
 */
CXRAPI_ATTR void CXRAPI_CALL
nv_cxr_get_runtime_version(uint32_t *major, uint32_t *minor, uint32_t *patch);

/*!
 * Result codes.
 */
typedef enum nv_cxr_result
{
	//! Operation succeeded.
	NV_CXR_SUCCESS = 0,

	//! Refer to logs for more information.
	NV_CXR_INTERNAL_SERVICE_ERROR = -1,

	//! Refer to logs for more information.
	NV_CXR_STARTUP_FAILED = -2,

	//! A pointer to an object, such as the service was null.
	NV_CXR_NULL_OBJECT = -3,

	//! A pointer to something, not an object was null.
	NV_CXR_NULL_PTR = -4,

	//! Can not call this function once the service has started, since the 1.0.1 version.
	NV_CXR_SERVICE_ALREADY_STARTED = -5,

	//! Can not call this function at this time, since the 1.0.1 version.
	NV_CXR_SERVICE_NOT_STARTED = -6,

	//! The property name is not currectly formed, since the 1.0.2 version.
	NV_CXR_PROPERTY_NAME_MALFORMED = -7,

	//! The property name is not a not known one for this type, since the 1.0.2 version.
	NV_CXR_PROPERTY_NAME_INVALID = -8,

	//! The value is not valid for this property, since the 1.0.3 version.
	NV_CXR_ERROR_PROPERTY_VALUE_INVALID = -9,

	//! The buffer provided is too small for the property value, since the 1.0.3 version.
	NV_CXR_ERROR_BUFFER_SIZE_INSUFFICIENT = -10,

	//! The property is read only, since the 1.0.5 version.
	NV_CXR_ERROR_PROPERTY_READ_ONLY = -11,

	//! The server could not open the port, since the 1.0.6 version.
	NV_CXR_PORT_UNAVAILABLE = -12,

	//! The property is write only, since the 1.0.7 version.
	NV_CXR_ERROR_PROPERTY_WRITE_ONLY = -13,
} nv_cxr_result_t;

/*!
 * Event types.
 */
typedef enum nv_cxr_event_type
{
	//! No event available.
	NV_CXR_EVENT_NONE = 0,

	//! The cloudxr client has connected.
	NV_CXR_EVENT_CLOUDXR_CLIENT_CONNECTED = 1,

	//! The cloudxr client has disconnected.
	NV_CXR_EVENT_CLOUDXR_CLIENT_DISCONNECTED = 2,

	//! The openxr app has connected.
	NV_CXR_EVENT_OPENXR_APP_CONNECTED = 3,

	//! The openxr app has disconnected.
	NV_CXR_EVENT_OPENXR_APP_DISCONNECTED = 4,
} nv_cxr_event_type_t;

/*!
 * Event, padded to 4096 bytes. Review the header file for details.
 */
typedef struct nv_cxr_event
{
	/// @cond INTERNAL_PADDING
	union {
		struct
		{
			/// @endcond
			//! The event type.
			nv_cxr_event_type_t type;
			/// @cond INTERNAL_PADDING
		};
		uint8_t _padding[4096];
	};
	/// @endcond
} nv_cxr_event_t;

/*!
 * This object encapsulates the entire runtime.
 */
struct nv_cxr_service;

/*!
 * Creates a service object, only one can be alive at a time. The runtime is
 * not started when this object is created, and instead the start function must
 * be used to get the runtime into a state accepting client connections and
 * apps being able to create a XrInstance in the OpenXR API.
 */
CXRAPI_ATTR nv_cxr_result_t CXRAPI_CALL
nv_cxr_service_create(struct nv_cxr_service **out_service);

/*!
 * Set a property to the given value, can only be set before start has been
 * called or after the runtime has been fully shutdown. If the runtime is
 * running the function will return @p NV_CXR_SERVICE_ALREADY_STARTED.
 *
 * Property names can only contain the symboles [a-z][0-9][-_], or the function
 * will return @p NV_CXR_PROPERTY_NAME_MALFORMED.
 *
 * Externally syncronized, no other threads must access this object.
 */
CXRAPI_ATTR nv_cxr_result_t CXRAPI_CALL
nv_cxr_service_set_string_property(struct nv_cxr_service *service,
                                   const char *property_name,
                                   size_t property_name_length,
                                   const char *value,
                                   size_t value_length);

/*!
 * Set a property to the given value, can only be set before start has been
 * called or after the runtime has been fully shutdown. If the runtime is
 * running the function will return @p NV_CXR_SERVICE_ALREADY_STARTED.
 *
 * Property names can only contain the symboles [a-z][0-9][-_], or the function
 * will return @p NV_CXR_PROPERTY_NAME_MALFORMED.
 *
 * Externally syncronized, no other threads must access this object.
 */
CXRAPI_ATTR nv_cxr_result_t CXRAPI_CALL
nv_cxr_service_set_boolean_property(struct nv_cxr_service *service,
                                    const char *property_name,
                                    size_t property_name_length,
                                    bool value);

/*!
 * Set a property to the given value, can only be set before start has been
 * called or after the runtime has been fully shutdown. If the runtime is
 * running the function will return @p NV_CXR_SERVICE_ALREADY_STARTED.
 *
 * Property names can only contain the symboles [a-z][0-9][-_], or the function
 * will return @p NV_CXR_PROPERTY_NAME_MALFORMED.
 *
 * Externally syncronized, no other threads must access this object.
 */
CXRAPI_ATTR nv_cxr_result_t CXRAPI_CALL
nv_cxr_service_set_int64_property(struct nv_cxr_service *service,
                                  const char *property_name,
                                  size_t property_name_length,
                                  int64_t value);

/*!
 * Get a string property value. The value is copied to the provided buffer.
 * If the buffer is too small, returns @p NV_CXR_ERROR_BUFFER_SIZE_INSUFFICIENT
 * and sets value_length to the required buffer size.
 *
 * Property names can only contain the symboles [a-z][0-9][-_], or the function
 * will return @p NV_CXR_PROPERTY_NAME_MALFORMED.
 *
 * Externally syncronized, no other threads must access this object.
 */
CXRAPI_ATTR nv_cxr_result_t CXRAPI_CALL
nv_cxr_service_get_string_property(struct nv_cxr_service *service,
                                   const char *property_name,
                                   size_t property_name_length,
                                   char *value,
                                   size_t *value_length);

/*!
 * Get a boolean property value.
 *
 * Property names can only contain the symboles [a-z][0-9][-_], or the function
 * will return @p NV_CXR_PROPERTY_NAME_MALFORMED.
 *
 * Externally syncronized, no other threads must access this object.
 */
CXRAPI_ATTR nv_cxr_result_t CXRAPI_CALL
nv_cxr_service_get_boolean_property(struct nv_cxr_service *service,
                                    const char *property_name,
                                    size_t property_name_length,
                                    bool *value);

/*!
 * Get an int64_t property value.
 *
 * Property names can only contain the symboles [a-z][0-9][-_], or the function
 * will return @p NV_CXR_PROPERTY_NAME_MALFORMED.
 *
 * Externally syncronized, no other threads must access this object.
 */
CXRAPI_ATTR nv_cxr_result_t CXRAPI_CALL
nv_cxr_service_get_int64_property(struct nv_cxr_service *service,
                                  const char *property_name,
                                  size_t property_name_length,
                                  int64_t *value);

/*!
 * Starts the service, currently this function only returns once the service
 * has fully completed starting up accepting connections from both clients and
 * applications. Returns NV_CXR_SERVICE_ALREADY_STARTED if the service has
 * already been started.
 *
 * Externally syncronized, no other threads must access this object.
 */
CXRAPI_ATTR nv_cxr_result_t CXRAPI_CALL
nv_cxr_service_start(struct nv_cxr_service *service);

/*!
 * Stops the service, disconnects all applications and clients. This function
 * only signals that the runtime should stop, use @ref nv_cxr_service_join to
 * wait for it to have fully stopped. Will return NV_CXR_SERVICE_NOT_STARTED if
 * the service hasn't been started, or has been fully stopped with a call to
 * the nv_cxr_service_join function.
 *
 * This function can be called after and before join returns. If the runtime
 * service is stopped by external factors and a call to join is happening, this
 * may cause NV_CXR_SERVICE_NOT_STARTED to be returned.
 */
CXRAPI_ATTR nv_cxr_result_t CXRAPI_CALL
nv_cxr_service_stop(struct nv_cxr_service *service);

/*!
 * Will return when the service has stopped, this is useful if using the
 * service's built in mechanisms to stop. Once this function returns the
 * service has fully stopped. Will return NV_CXR_SERVICE_NOT_STARTED if the
 * service hasn't been started.
 *
 * Externally syncronized, no other thread must access this object, except for
 * stop, see comment on that function.
 */
CXRAPI_ATTR nv_cxr_result_t CXRAPI_CALL
nv_cxr_service_join(struct nv_cxr_service *service);

/*!
 * Destroys a service object, the service must been in a fully stop state,
 * either by never calling start, or after a series of successful calls to start
 * and join.
 *
 * Externally syncronized, no other thread must access this object.
 */
CXRAPI_ATTR void CXRAPI_CALL
nv_cxr_service_destroy(struct nv_cxr_service *service);

/*!
 * Returns the oldest event that has occurred since the start of the service.
 * Each call returns one event and removes it from the internal event queue.
 * If no events are available, returns NV_CXR_EVENT_NONE.
 *
 * @param service The service object.
 * @param event Pointer to store the retrieved event.
 * @return NV_CXR_SUCCESS if an event was retrieved, NV_CXR_EVENT_NONE if no events available, or error code.
 */
CXRAPI_ATTR nv_cxr_result_t CXRAPI_CALL
nv_cxr_service_poll_event(struct nv_cxr_service *service, nv_cxr_event_t *event);

/*!
 * Update the client authentication token while the service is running.
 * Changes will immediately affect new client connections, existing connections
 * are not affected. Once this function has been used once it will override the
 * deprecated property version of this functionality.
 *
 * Pass an empty token (token_length = 0 or token = nullptr) to disable token verification.
 *
 * Thread-safe, can be called from multiple threads.
 *
 * Since the 1.0.7 version.
 *
 * @param service The service object.
 * @param token Pointer to the new token string, or nullptr to disable verification.
 * @param token_length Length of the token string in bytes.
 * @return NV_CXR_SUCCESS on success, NV_CXR_SERVICE_NOT_STARTED if the service
 *         is not running, or other error code on failure.
 */
CXRAPI_ATTR nv_cxr_result_t CXRAPI_CALL
nv_cxr_update_client_token(struct nv_cxr_service *service, const char *token, size_t token_length);

/*
 * Function pointer types for the CXR API.
 */
// clang-format off
typedef void(CXRAPI_PTR *PFN_nv_cxr_get_library_api_version)(uint32_t *, uint32_t *, uint32_t *);
typedef void(CXRAPI_PTR *PFN_nv_cxr_get_runtime_version)(uint32_t *, uint32_t *, uint32_t *);
typedef nv_cxr_result_t(CXRAPI_PTR *PFN_nv_cxr_service_create)(struct nv_cxr_service **);
typedef nv_cxr_result_t(CXRAPI_PTR *PFN_nv_cxr_service_set_string_property)(struct nv_cxr_service *, const char *, size_t, const char *, size_t);
typedef nv_cxr_result_t(CXRAPI_PTR *PFN_nv_cxr_service_set_boolean_property)(struct nv_cxr_service *, const char *, size_t, bool);
typedef nv_cxr_result_t(CXRAPI_PTR *PFN_nv_cxr_service_set_int64_property)(struct nv_cxr_service *, const char *, size_t, int64_t);
typedef nv_cxr_result_t(CXRAPI_PTR *PFN_nv_cxr_service_get_string_property)(struct nv_cxr_service *, const char *, size_t, char *, size_t *);
typedef nv_cxr_result_t(CXRAPI_PTR *PFN_nv_cxr_service_get_boolean_property)(struct nv_cxr_service *, const char *, size_t, bool *);
typedef nv_cxr_result_t(CXRAPI_PTR *PFN_nv_cxr_service_get_int64_property)(struct nv_cxr_service *, const char *, size_t, int64_t *);
typedef nv_cxr_result_t(CXRAPI_PTR *PFN_nv_cxr_service_start)(struct nv_cxr_service *);
typedef nv_cxr_result_t(CXRAPI_PTR *PFN_nv_cxr_service_stop)(struct nv_cxr_service *);
typedef nv_cxr_result_t(CXRAPI_PTR *PFN_nv_cxr_service_join)(struct nv_cxr_service *);
typedef void(CXRAPI_PTR *PFN_nv_cxr_service_destroy)(struct nv_cxr_service *);
typedef nv_cxr_result_t(CXRAPI_PTR *PFN_nv_cxr_service_poll_event)(struct nv_cxr_service *, nv_cxr_event_t *);
typedef nv_cxr_result_t(CXRAPI_PTR *PFN_nv_cxr_update_client_token)(struct nv_cxr_service *, const char *, size_t);
// clang-format on

#ifdef __cplusplus
}
#endif
