// lx_prelude.js - globalThis.lx object definition
// This script is executed before user scripts to set up the lx environment.
// It relies on Go-layer polyfills: fetch, crypto, zlib, Buffer, console, setTimeout, etc.
//
// Reference: plugins/mimusic-plugin-lxmusic/lxserver/src/server/userApi.ts

'use strict';

// Event handler registry (mirrors Node.js eventHandlers Map)
var _eventHandlers = new Map();

// Registered sources from script's lx.send('inited', {sources})
var _registeredSources = {};

// Script metadata (populated by Go side before script execution)
var _scriptInfo = {
    name: '',
    description: '',
    version: '',
    author: '',
    homepage: '',
    rawScript: ''
};

globalThis.lx = {
    version: '2.0.0',
    env: 'desktop',
    platform: 'web',
    currentScriptInfo: _scriptInfo,

    EVENT_NAMES: {
        request: 'request',
        inited: 'inited',
        updateAlert: 'updateAlert'
    },

    // Utility functions wrapping Go-layer polyfills
    utils: {
        buffer: {
            from: function(data, encoding) {
                return Buffer.from(data, encoding);
            },
            bufToString: function(buf, format) {
                if (typeof buf === 'object' && buf !== null && typeof buf.toString === 'function') {
                    return buf.toString(format || 'utf8');
                }
                return Buffer.from(buf, 'binary').toString(format || 'utf8');
            }
        },
        crypto: {
            md5: function(str) {
                return crypto.md5(str || '');
            },
            aesEncrypt: function(buffer, mode, key, iv) {
                return crypto.aesEncrypt(buffer, mode, key, iv);
            },
            rsaEncrypt: function(buffer, key) {
                return crypto.rsaEncrypt(buffer, key);
            },
            randomBytes: function(size) {
                return crypto.randomBytes(size);
            }
        },
        zlib: {
            inflate: function(buffer) {
                return zlib.inflate(buffer);
            },
            deflate: function(buffer) {
                return zlib.deflate(buffer);
            }
        }
    },

    // HTTP request — callback-style API matching LX Music Desktop's lx.request
    // Signature: lx.request(url, options, callback)
    //   callback(error, response, body)
    //   response = { statusCode, statusMessage, headers, body }
    // Returns a cancel function.
    request: function(url, options, callback) {
        if (typeof options === 'function') {
            callback = options;
            options = {};
        }
        options = options || {};

        var method = (options.method || 'GET').toUpperCase();
        var timeout = (typeof options.timeout === 'number' && options.timeout > 0)
            ? Math.min(options.timeout, 60000)
            : 60000;
        var headers = options.headers || {};

        // Determine body: support body, form, formData
        var bodyContent = options.body || null;
        if (options.form) {
            bodyContent = options.form;
            if (typeof bodyContent === 'object') {
                var parts = [];
                var formKeys = Object.keys(bodyContent);
                for (var fi = 0; fi < formKeys.length; fi++) {
                    parts.push(encodeURIComponent(formKeys[fi]) + '=' + encodeURIComponent(bodyContent[formKeys[fi]]));
                }
                bodyContent = parts.join('&');
                if (!headers['Content-Type'] && !headers['content-type']) {
                    headers['Content-Type'] = 'application/x-www-form-urlencoded';
                }
            }
        } else if (options.formData) {
            bodyContent = options.formData;
        }

        // If body is an object (not string), JSON-stringify it
        if (bodyContent !== null && typeof bodyContent === 'object') {
            bodyContent = JSON.stringify(bodyContent);
            if (!headers['Content-Type'] && !headers['content-type']) {
                headers['Content-Type'] = 'application/json';
            }
        }

        var aborted = false;
        var callbackCalled = false;

        var fetchOptions = {
            method: method,
            headers: headers
        };
        if (bodyContent) {
            fetchOptions.body = bodyContent;
        }

        function safeCallback(err, response, body) {
            if (callbackCalled || aborted) return;
            callbackCalled = true;
            if (typeof callback === 'function') {
                try {
                    callback(err, response, body);
                } catch (callbackError) {
                    console.error('[lx.request] callback threw:', callbackError);
                }
            }
        }

        fetch(url, fetchOptions).then(function(resp) {
            if (aborted) return;
            console.error('[lx.request] fetch resolved, status=' + resp.status + ' url=' + url.substring(0, 100));
            return resp.text().then(function(bodyText) {
                if (aborted) return;
                console.error('[lx.request] resp.text() resolved, bodyLen=' + (bodyText ? bodyText.length : 0));
                // Try to parse body as JSON (matching needle behavior)
                var parsedBody = bodyText;
                try {
                    parsedBody = JSON.parse(bodyText);
                } catch (parseError) {
                    // Keep as string
                }

                var response = {
                    statusCode: resp.status,
                    statusMessage: resp.statusText || '',
                    headers: resp.headers || {},
                    body: parsedBody
                };

                console.error('[lx.request] calling safeCallback, statusCode=' + response.statusCode);
                safeCallback(null, response, parsedBody);
                console.error('[lx.request] safeCallback returned');
            });
        }).catch(function(err) {
            if (aborted) return;
            var errMsg = (err && err.message) ? err.message : String(err);
            console.error('[lx.request] fetch/text catch: ' + errMsg);
            safeCallback(new Error(errMsg), null, null);
        });

        // Return cancel function
        return function() {
            aborted = true;
        };
    },

    // Send event to Go side via __cqjs_send bridge function
    send: function(eventName, data) {
        if (eventName === 'inited') {
            if (data && data.sources) {
                _registeredSources = data.sources;
            }
        }
        if (typeof __cqjs_send === 'function') {
            __cqjs_send(eventName, JSON.stringify(data));
        }
    },

    // Register event handler
    on: function(eventName, handler) {
        _eventHandlers.set(eventName, handler);
    },

    // Dispatch event to registered handler (called from Go side via dispatch request).
    // The handler may return a Promise; the resolved/rejected value is sent back
    // to Go via __cqjs_send('dispatchResult', ...) / __cqjs_send('dispatchError', ...).
    // A 25-second timeout protects against handlers whose Promise never settles.
    _dispatch: function(requestId, eventName, data) {
        var handler = _eventHandlers.get(eventName);
        if (typeof handler !== 'function') {
            if (typeof __cqjs_send === 'function') {
                __cqjs_send('dispatchError', JSON.stringify({
                    id: requestId,
                    error: 'No handler registered for event: ' + eventName
                }));
            }
            return;
        }

        var settled = false;

        function sendResult(value) {
            if (settled) return;
            settled = true;
            console.error('[_dispatch] sendResult called, requestId=' + requestId + ' value=' + (typeof value === 'string' ? value.substring(0, 200) : String(value)));
            if (typeof __cqjs_send === 'function') {
                __cqjs_send('dispatchResult', JSON.stringify({
                    id: requestId,
                    result: value
                }));
            }
        }

        function sendError(err) {
            if (settled) return;
            settled = true;
            var errMsg = (err && err.message) ? err.message : String(err);
            console.error('[_dispatch] sendError called, requestId=' + requestId + ' error=' + errMsg);
            if (typeof __cqjs_send === 'function') {
                __cqjs_send('dispatchError', JSON.stringify({
                    id: requestId,
                    error: errMsg
                }));
            }
        }

        try {
            var result = handler(data);
            var isThenable = (result && typeof result.then === 'function');
            console.error('[_dispatch] handler returned, isThenable=' + isThenable + ' requestId=' + requestId);

            if (isThenable) {
                var timeoutId = setTimeout(function() {
                    console.error('[_dispatch] TIMEOUT fired, settled=' + settled + ' requestId=' + requestId);
                    sendError(new Error('dispatch timeout: handler Promise did not settle within 18s'));
                }, 18000);

                result.then(function(value) {
                    console.error('[_dispatch] Promise resolved, requestId=' + requestId);
                    clearTimeout(timeoutId);
                    sendResult(value);
                }, function(err) {
                    console.error('[_dispatch] Promise rejected, requestId=' + requestId);
                    clearTimeout(timeoutId);
                    sendError(err);
                });
            } else {
                sendResult(result);
            }
        } catch (err) {
            sendError(err);
        }
    },

    // Get registered sources (used by Go side after 'inited' event)
    _getSources: function() {
        return _registeredSources;
    }
};

// Browser-like global aliases (needed by obfuscated scripts)
globalThis.window = globalThis;
globalThis.global = globalThis;
