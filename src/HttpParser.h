/*
 * Authored by Alex Hultman, 2018-2020.
 * Intellectual property of third-party.

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef UWS_HTTPPARSER_H
#define UWS_HTTPPARSER_H

// todo: HttpParser is in need of a few clean-ups and refactorings

/* The HTTP parser is an independent module subject to unit testing / fuzz testing */

#include <string>
#include <cstring>
#include <algorithm>
#include "MoveOnlyFunction.h"
#include "ChunkedEncoding.h"

#include "BloomFilter.h"
#include "ProxyParser.h"
#include "QueryParser.h"

namespace uWS {

/* We require at least this much post padding */
static const unsigned int MINIMUM_HTTP_POST_PADDING = 32;

struct HttpRequest {

    friend struct HttpParser;

private:
    const static int MAX_HEADERS = 50;
    struct Header {
        std::string_view key, value;
    } headers[MAX_HEADERS];
    bool ancientHttp;
    unsigned int querySeparator;
    bool didYield;
    BloomFilter bf;
    std::pair<int, std::string_view *> currentParameters;

public:
    bool isAncient() {
        return ancientHttp;
    }

    bool getYield() {
        return didYield;
    }

    /* Iteration over headers (key, value) */
    struct HeaderIterator {
        Header *ptr;

        bool operator!=(const HeaderIterator &other) const {
            /* Comparison with end is a special case */
            if (ptr != other.ptr) {
                return other.ptr || ptr->key.length();
            }
            return false;
        }

        HeaderIterator &operator++() {
            ptr++;
            return *this;
        }

        std::pair<std::string_view, std::string_view> operator*() const {
            return {ptr->key, ptr->value};
        }
    };

    HeaderIterator begin() {
        return {headers + 1};
    }

    HeaderIterator end() {
        return {nullptr};
    }

    /* If you do not want to handle this route */
    void setYield(bool yield) {
        didYield = yield;
    }

    std::string_view getHeader(std::string_view lowerCasedHeader) {
        if (bf.mightHave(lowerCasedHeader)) {
            for (Header *h = headers; (++h)->key.length(); ) {
                if (h->key.length() == lowerCasedHeader.length() && !strncmp(h->key.data(), lowerCasedHeader.data(), lowerCasedHeader.length())) {
                    return h->value;
                }
            }
        }
        return std::string_view(nullptr, 0);
    }

    std::string_view getUrl() {
        return std::string_view(headers->value.data(), querySeparator);
    }

    std::string_view getMethod() {
        return std::string_view(headers->key.data(), headers->key.length());
    }

    /* Returns the raw querystring as a whole, still encoded */
    std::string_view getQuery() {
        if (querySeparator < headers->value.length()) {
            /* Strip the initial ? */
            return std::string_view(headers->value.data() + querySeparator + 1, headers->value.length() - querySeparator - 1);
        } else {
            return std::string_view(nullptr, 0);
        }
    }

    /* Finds and decodes the URI component. */
    std::string_view getQuery(std::string_view key) {
        /* Raw querystring including initial '?' sign */
        std::string_view queryString = std::string_view(headers->value.data() + querySeparator, headers->value.length() - querySeparator);

        return getDecodedQueryValue(key, queryString);
    }

    void setParameters(std::pair<int, std::string_view *> parameters) {
        currentParameters = parameters;
    }

    std::string_view getParameter(unsigned short index) {
        if (currentParameters.first < (int) index) {
            return {};
        } else {
            return currentParameters.second[index];
        }
    }

};

struct HttpParser {

private:
    std::string fallback;
    /* This guy really has only 30 bits since we reserve two highest bits to chunked encoding parsing state */
    unsigned int remainingStreamingBytes = 0;

    const size_t MAX_FALLBACK_SIZE = 1024 * 4;

    static unsigned int toUnsignedInteger(std::string_view str) {
        unsigned int unsignedIntegerValue = 0;
        for (char c : str) {
            unsignedIntegerValue = unsignedIntegerValue * 10u + ((unsigned int) c - (unsigned int) '0');
        }
        return unsignedIntegerValue;
    }
    
    /* Find carriage return will scan forever. But we "fence" the end margin part of the receive buffer,
     * by putting a CR there in case one isn't found before it, so this optimizing assumption is fine. */
    static inline void *find_cr(char *p, char */*end*/) {
        for (uint64_t mask = 0x0d0d0d0d0d0d0d0d; true; p += 8) {
            uint64_t val = *(uint64_t *)p ^ mask;
            if ((val + 0xfefefefefefefeffull) & (~val & 0x8080808080808080ull)) {
                while (*(unsigned char *)p != 0x0d) p++;
                return (void *)p;
            }
        }
    }

    /* End is only used for the proxy parser. The HTTP parser recognizes "\ra" as invalid "\r\n" scan and breaks. */
    static unsigned int getHeaders(char *postPaddedBuffer, char *end, struct HttpRequest::Header *headers, void *reserved) {
        char *preliminaryKey, *preliminaryValue, *start = postPaddedBuffer;

        #ifdef UWS_WITH_PROXY
            /* ProxyParser is passed as reserved parameter */
            ProxyParser *pp = (ProxyParser *) reserved;

            /* Parse PROXY protocol */
            auto [done, offset] = pp->parse({start, (size_t) (end - postPaddedBuffer)});
            if (!done) {
                /* We do not reset the ProxyParser (on filure) since it is tied to this
                * connection, which is really only supposed to ever get one PROXY frame
                * anyways. We do however allow multiple PROXY frames to be sent (overwrites former). */
                return 0;
            } else {
                /* We have consumed this data so skip it */
                start += offset;
            }
        #else
            /* This one is unused */
            (void) reserved;
        #endif

        /* It is critical for fallback buffering logic that we only return with success
         * if we managed to parse a complete HTTP request (minus data). Returning success
         * for PROXY means we can end up succeeding, yet leaving bytes in the fallback buffer
         * which is then removed, and our counters to flip due to overflow and we end up with a crash */

        for (unsigned int i = 0; i < HttpRequest::MAX_HEADERS - 1; i++) {
            /* Lower case and short scan until ':', or stop at \r (from previous scan) */
            for (preliminaryKey = postPaddedBuffer; (*postPaddedBuffer != ':') && (*(unsigned char *)postPaddedBuffer > 32); *(postPaddedBuffer++) |= 32);
            headers->key = std::string_view(preliminaryKey, (size_t) (postPaddedBuffer - preliminaryKey));
            /* Assume colon, space follows (this is fine as we have at least 2 bytes past) */
            if (postPaddedBuffer[0] == ':' && postPaddedBuffer[1] == ' ') {
                postPaddedBuffer += 2;
            } else {
                /* Trim until value starts */
                for (; (*postPaddedBuffer == ':' || *(unsigned char *)postPaddedBuffer < 33) && *postPaddedBuffer != '\r'; postPaddedBuffer++);
            }
            preliminaryValue = postPaddedBuffer;
            /* The goal of this call is to find next "\r\n", fast */
            postPaddedBuffer = (char *) find_cr(postPaddedBuffer, end);
            /* We fence end[0] with \r, followed by end[1] being something that is "not \n", to signify "not found".
                * This way we can have this one single check to see if we found \r\n WITHIN our allowed search space. */
            if (postPaddedBuffer[1] == '\n') {
                /* Store this header, it is valid */
                headers->value = std::string_view(preliminaryValue, (size_t) (postPaddedBuffer - preliminaryValue));
                postPaddedBuffer += 2;
                headers++;

                /* We definitely have at least one header (or request line), so check if we are done */
                if (*postPaddedBuffer == '\r') {
                    if (postPaddedBuffer[1] == '\n') {
                        /* This cann take the very last header space */
                        headers->key = std::string_view(nullptr, 0);
                        return (unsigned int) ((postPaddedBuffer + 2) - start);
                    } else {
                        /* \r\n\r plus non-\n letter is malformed request, or simply out of search space */
                        return 0;
                    }
                }
            } else {
                /* We are either out of search space or this is a malformed request */
                return 0;
            }
        }
        /* We ran out of header space, too large request */
        return 0;
    }

    // the only caller of getHeaders
    template <int CONSUME_MINIMALLY>
    std::pair<unsigned int, void *> fenceAndConsumePostPadded(char *data, unsigned int length, void *user, void *reserved, HttpRequest *req, MoveOnlyFunction<void *(void *, HttpRequest *)> &requestHandler, MoveOnlyFunction<void *(void *, std::string_view, bool)> &dataHandler) {

        /* How much data we CONSUMED (to throw away) */
        unsigned int consumedTotal = 0;

        /* Fence two bytes past end of our buffer (buffer has post padded margins).
         * This is to always catch scan for \r but not for \r\n. */
        data[length] = '\r';
        data[length + 1] = 'a'; /* Anything that is not \n, to trigger "invalid request" */

        for (unsigned int consumed; length && (consumed = getHeaders(data, data + length, req->headers, reserved)); ) {
            data += consumed;
            length -= consumed;
            consumedTotal += consumed;

            /* Store HTTP version (ancient 1.0 or 1.1) */
            req->ancientHttp = req->headers->value.length() && (req->headers->value[req->headers->value.length() - 1] == '0');

            /* Strip away tail of first "header value" aka URL */
            req->headers->value = std::string_view(req->headers->value.data(), (size_t) std::max<int>(0, (int) req->headers->value.length() - 9));

            /* Add all headers to bloom filter */
            req->bf.reset();
            for (HttpRequest::Header *h = req->headers; (++h)->key.length(); ) {
                req->bf.add(h->key);
            }

            /* Parse query */
            const char *querySeparatorPtr = (const char *) memchr(req->headers->value.data(), '?', req->headers->value.length());
            req->querySeparator = (unsigned int) ((querySeparatorPtr ? querySeparatorPtr : req->headers->value.data() + req->headers->value.length()) - req->headers->value.data());

            /* If returned socket is not what we put in we need
             * to break here as we either have upgraded to
             * WebSockets or otherwise closed the socket. */
            void *returnedUser = requestHandler(user, req);
            if (returnedUser != user) {
                /* We are upgraded to WebSocket or otherwise broken */
                return {consumedTotal, returnedUser};
            }

            
            if (req->getMethod() != "get") {
                std::string_view contentLengthString = req->getHeader("content-length");
                if (contentLengthString.length()) {
                    remainingStreamingBytes = toUnsignedInteger(contentLengthString);

                    if (!CONSUME_MINIMALLY) {
                        unsigned int emittable = std::min<unsigned int>(remainingStreamingBytes, length);
                        dataHandler(user, std::string_view(data, emittable), emittable == remainingStreamingBytes);
                        remainingStreamingBytes -= emittable;

                        data += emittable;
                        length -= emittable;
                        consumedTotal += emittable;
                    }
                } else {
                    /* We are not GET and we have no content-length, so assume transfer-encoding: chunked */
                    remainingStreamingBytes = STATE_IS_CHUNKED;
                    /* If consume minimally, we do not want to consume anything but we want to mark this as being chunked */
                    if (!CONSUME_MINIMALLY) {
                        /* Go ahead and parse it (todo: better heuristics for emitting FIN to the app level) */
                        std::string_view dataToConsume(data, length);
                        for (auto chunk : uWS::ChunkIterator(&dataToConsume, &remainingStreamingBytes)) {
                            dataHandler(user, chunk, chunk.length() == 0);
                        }
                        unsigned int consumed = (length - (unsigned int) dataToConsume.length());
                        data = (char *) dataToConsume.data();
                        length = (unsigned int) dataToConsume.length();
                        consumedTotal += consumed;
                    }
                }
            } else {
                /* Still emit an empty data chunk to signal no data */
                dataHandler(user, {}, true);
            }

            if (CONSUME_MINIMALLY) {
                break;
            }
        }
        return {consumedTotal, user};
    }

public:
    void *consumePostPadded(char *data, unsigned int length, void *user, void *reserved, MoveOnlyFunction<void *(void *, HttpRequest *)> &&requestHandler, MoveOnlyFunction<void *(void *, std::string_view, bool)> &&dataHandler, MoveOnlyFunction<void *(void *)> &&errorHandler) {

        /* This resets BloomFilter by construction, but later we also reset it again.
         * Optimize this to skip resetting twice (req could be made global) */
        HttpRequest req;

        if (remainingStreamingBytes) {

            /* It's either chunked or with a content-length */
            if (isParsingChunkedEncoding(remainingStreamingBytes)) {
                std::string_view dataToConsume(data, length);
                for (auto chunk : uWS::ChunkIterator(&dataToConsume, &remainingStreamingBytes)) {
                    dataHandler(user, chunk, chunk.length() == 0);
                }
                data = (char *) dataToConsume.data();
                length = (unsigned int) dataToConsume.length();
            } else {
                // this is exactly the same as below!
                // todo: refactor this
                if (remainingStreamingBytes >= length) {
                    void *returnedUser = dataHandler(user, std::string_view(data, length), remainingStreamingBytes == length);
                    remainingStreamingBytes -= length;
                    return returnedUser;
                } else {
                    void *returnedUser = dataHandler(user, std::string_view(data, remainingStreamingBytes), true);

                    data += remainingStreamingBytes;
                    length -= remainingStreamingBytes;

                    remainingStreamingBytes = 0;

                    if (returnedUser != user) {
                        return returnedUser;
                    }
                }
            }

        } else if (fallback.length()) {
            unsigned int had = (unsigned int) fallback.length();

            size_t maxCopyDistance = std::min<size_t>(MAX_FALLBACK_SIZE - fallback.length(), (size_t) length);

            /* We don't want fallback to be short string optimized, since we want to move it */
            fallback.reserve(fallback.length() + maxCopyDistance + std::max<unsigned int>(MINIMUM_HTTP_POST_PADDING, sizeof(std::string)));
            fallback.append(data, maxCopyDistance);

            // break here on break
            std::pair<unsigned int, void *> consumed = fenceAndConsumePostPadded<true>(fallback.data(), (unsigned int) fallback.length(), user, reserved, &req, requestHandler, dataHandler);
            if (consumed.second != user) {
                return consumed.second;
            }

            if (consumed.first) {

                /* This logic assumes that we consumed everything in fallback buffer.
                 * This is critically important, as we will get an integer overflow in case
                 * of "had" being larger than what we consumed, and that we would drop data */
                fallback.clear();
                data += consumed.first - had;
                length -= consumed.first - had;

                if (remainingStreamingBytes) {
                    // this is exactly the same as above!
                    if (remainingStreamingBytes >= (unsigned int) length) {
                        void *returnedUser = dataHandler(user, std::string_view(data, length), remainingStreamingBytes == (unsigned int) length);
                        remainingStreamingBytes -= length;
                        return returnedUser;
                    } else {
                        void *returnedUser = dataHandler(user, std::string_view(data, remainingStreamingBytes), true);

                        data += remainingStreamingBytes;
                        length -= remainingStreamingBytes;

                        remainingStreamingBytes = 0;

                        if (returnedUser != user) {
                            return returnedUser;
                        }
                    }
                }

            } else {
                if (fallback.length() == MAX_FALLBACK_SIZE) {
                    // note: you don't really need error handler, just return something strange!
                    // we could have it return a constant pointer to denote error!
                    return errorHandler(user);
                }
                return user;
            }
        }

        std::pair<unsigned int, void *> consumed = fenceAndConsumePostPadded<false>(data, length, user, reserved, &req, requestHandler, dataHandler);
        if (consumed.second != user) {
            return consumed.second;
        }

        data += consumed.first;
        length -= consumed.first;

        if (length) {
            if (length < MAX_FALLBACK_SIZE) {
                fallback.append(data, length);
            } else {
                return errorHandler(user);
            }
        }

        // added for now
        return user;
    }
};

}

#endif // UWS_HTTPPARSER_H
