/**********************************************************\
Original Author: Richard Bateman

Created:    Jan 24, 2011
License:    Dual license model; choose one of two:
            New BSD License
            http://www.opensource.org/licenses/bsd-license.php
            - or -
            GNU Lesser General Public License, version 2.1
            http://www.gnu.org/licenses/lgpl-2.1.html

Copyright 2011 Richard Bateman,
               Firebreath development team
\**********************************************************/

#include "BrowserHost.h"
#include <boost/algorithm/string.hpp>
#include <boost/bind.hpp>
#include "precompiled_headers.h" // On windows, everything above this line in PCH

#include "BrowserStreamRequest.h"
#include "SimpleStreamHelper.h"

static const int MEGABYTE = 1024 * 1024;

FB::SimpleStreamHelperPtr FB::SimpleStreamHelper::AsyncGet( const FB::BrowserHostPtr& host, const FB::URI& uri,
    const HttpCallback& callback, bool cache /*= true*/, size_t bufferSize /*= 256*1024*/ )
{
    BrowserStreamRequest req(uri, "GET");
    req.setCallback(callback);
    req.setBufferSize(bufferSize);
    req.setCacheable(cache);
    return AsyncRequest(host, req);
}

FB::SimpleStreamHelperPtr FB::SimpleStreamHelper::AsyncPost(const FB::BrowserHostPtr& host,
                                                            const FB::URI& uri,
                                                            const std::string& postdata,
                                                            const HttpCallback& callback,
                                                            bool cache /*= true*/,
                                                            size_t bufferSize /*= 256*1024*/ )
{
    BrowserStreamRequest req(uri, "POST");
    req.setPostData(postdata);
    req.setCallback(callback);
    req.setBufferSize(bufferSize);
    req.setCacheable(cache);
    return AsyncRequest(host, req);
}

FB::SimpleStreamHelperPtr FB::SimpleStreamHelper::AsyncRequest( const FB::BrowserHostConstPtr& host,
                                                                const BrowserStreamRequest& req ) {
    if (!req.getCallback()) {
        throw std::runtime_error("Invalid callback");
    }
    if (!host->isMainThread()) {
        // This must be run from the main thread
        return host->CallOnMainThread(boost::bind(&AsyncRequest, host, req));
    }
    FB::BrowserStreamPtr stream(host->createStream(req, false));
    return AsyncRequest(host, stream, req);
}

FB::SimpleStreamHelperPtr FB::SimpleStreamHelper::AsyncRequest( const FB::BrowserHostConstPtr& host,
                                                                const FB::BrowserStreamPtr& stream,
                                                                const BrowserStreamRequest& req ) {
    if (!host->isMainThread()) {
        // This must be run from the main thread
        return host->CallOnMainThread(boost::bind(&AsyncRequest, host, stream, req));
    }
    FB::SimpleStreamHelperPtr ptr(boost::make_shared<FB::SimpleStreamHelper>(req.getCallback(), req.internalBufferSize));
    // This is kinda a weird trick; it's responsible for freeing itself, unless something decides
    // to hold a reference to it.
    ptr->keepReference(ptr);
    stream->AttachObserver(ptr);
    return ptr;
}

struct SyncHTTPHelper
{
public:
    SyncHTTPHelper()
        : done(false) { }
    void setPtr(const FB::SimpleStreamHelperPtr& inPtr) { ptr = inPtr; }

    void getURLCallback(bool success, const FB::HeaderMap& headers,
        const boost::shared_array<uint8_t>& data, const size_t size, float progress, bool isCompleted)
    {
        boost::lock_guard<boost::mutex> lock(m_mutex);
        m_response = boost::make_shared<FB::HttpStreamResponse>(success, headers, data, size);
        done = true;
        m_cond.notify_all();
    }
    void waitForDone() {
        boost::unique_lock<boost::mutex> lock(m_mutex);
        while (!done) {
            m_cond.wait(lock);
        }
    }

    bool done;
    FB::SimpleStreamHelperPtr ptr;
    boost::condition_variable m_cond;
    boost::mutex m_mutex;
    FB::HttpStreamResponsePtr m_response;
};


FB::HttpStreamResponsePtr FB::SimpleStreamHelper::SynchronousRequest( const FB::BrowserHostPtr& host, const BrowserStreamRequest& req )
{
    // We can't ever block on the main thread, so SynchronousGet can't be called from there.
    // Also, if you could block the main thread, that still wouldn't work because the request
    // is processed on the main thread!
    assert(!host->isMainThread());
    SyncHTTPHelper helper;
    try {

        FB::HttpCallback cb(boost::bind(&SyncHTTPHelper::getURLCallback, &helper, _1, _2, _3, _4, _5, _6));
	    FB::BrowserStreamRequest req2(req);
 	    req2.setCallback(cb);
        FB::SimpleStreamHelperPtr ptr = AsyncRequest(host, req2);

        helper.setPtr(ptr);
        helper.waitForDone();
    } catch (const std::exception&) {
        // If anything weird happens, just return NULL (to indicate failure)
        return FB::HttpStreamResponsePtr();
    }
    return helper.m_response;
}

FB::HttpStreamResponsePtr FB::SimpleStreamHelper::SynchronousGet( const FB::BrowserHostPtr& host,
    const FB::URI& uri, const bool cache /*= true*/, const size_t bufferSize /*= 128*1024*/ )
{
    FB::BrowserStreamRequest req(uri, "GET");
    req.setCacheable(cache);
    req.setBufferSize(bufferSize);
    return SynchronousRequest(host, req);
}

FB::HttpStreamResponsePtr FB::SimpleStreamHelper::SynchronousPost( const FB::BrowserHostPtr& host,
    const FB::URI& uri, const std::string& postdata, const bool cache /*= true*/, const size_t bufferSize /*= 128*1024*/ )
{
    FB::BrowserStreamRequest req(uri, "POST");
    req.setCacheable(cache);
    req.setBufferSize(bufferSize);
    req.setPostData(postdata);
    return SynchronousRequest(host, req);
}

FB::SimpleStreamHelper::SimpleStreamHelper( const HttpCallback& callback, const size_t blockSize )
    : blockSize(blockSize), received(0), callback(callback)
{

}

bool FB::SimpleStreamHelper::onStreamCompleted( FB::StreamCompletedEvent *evt, FB::BrowserStream *stream )
{
    if (!evt->success) {
        if (callback)
            callback(false, FB::HeaderMap(), boost::shared_array<uint8_t>(), received, 0.0, true);
        callback.clear();
        self.reset();
        return false;
    }
    if (!data) {
        data = boost::shared_array<uint8_t>(new uint8_t[received]);
        int i = 0;
        for (BlockList::const_iterator it = blocks.begin();
            it != blocks.end(); ++it) {
            size_t offset(i * blockSize);
            size_t len(received - offset);
            if (len > blockSize)
                len = blockSize;

            std::copy(it->get(), it->get()+len, data.get()+offset);
            ++i;
        }
        // Free all the old blocks
        blocks.clear();
    }
    if (callback && stream) {
        std::multimap<std::string, std::string> headers;
        headers = parse_http_headers(stream->getHeaders());
        callback(true, headers, data, received, 0.0, true);
    }
    callback.clear();
    self.reset();
    return false; // Always return false to make sure the browserhost knows to let go of the object
}

bool FB::SimpleStreamHelper::onStreamOpened( FB::StreamOpenedEvent *evt, FB::BrowserStream * )
{
    // We can't reliably find the actual length, so we won't try
    return false;
}

bool FB::SimpleStreamHelper::onStreamDataArrived( FB::StreamDataArrivedEvent *evt, FB::BrowserStream * )
{
    received += evt->getLength();
    const uint8_t* buf = reinterpret_cast<const uint8_t*>(evt->getData());
    const uint8_t* endbuf = buf + evt->getLength();

    int len = evt->getLength();
    int offset = evt->getDataPosition();
    float progress =  evt->getProgress();

    while (buf < endbuf) {
        size_t n = offset / blockSize;
        size_t pos = offset % blockSize;
        if (blocks.size() < n+1) {
            blocks.push_back(boost::shared_array<uint8_t>(new uint8_t[blockSize]));
        }
        uint8_t *destBuf = blocks.back().get();
        //if (pos + len > )
        int curLen = len;
        if (pos + len >= blockSize) {
            // If there isn't room in the current block, copy what there is room for
            // and loop
            curLen = blockSize-pos;
        }
        // Copy the bytes that fit in this buffer
        std::copy(buf, buf+curLen, destBuf+pos);
        buf += curLen;
        offset += curLen;
        len -= curLen;
    }
    if (callback) {
        std::multimap<std::string, std::string> dummyHeaders;
        boost::shared_array<uint8_t> dummyData;
        callback(true,dummyHeaders,dummyData,0,progress,false);
    }
    return false;
}

FB::HeaderMap FB::SimpleStreamHelper::parse_http_headers(const std::string& headers )
{
    FB::HeaderMap res;
    std::vector<std::string> lines;
    boost::split(lines, headers, boost::is_any_of("\r\n"));
    for (std::vector<std::string>::const_iterator it = lines.begin(); it != lines.end(); ++it) {
        std::string line = boost::trim_copy(*it);
        if (line.empty()) continue;
        size_t loc = line.find(':');
        if (loc == std::string::npos) {
            // Weird; bad header
            continue;
        }
        res.insert(std::make_pair(boost::trim_copy(line.substr(0, loc)),
            boost::trim_copy(line.substr(loc + 1))));
    }
    return res;
}

void FB::SimpleStreamHelper::keepReference( const SimpleStreamHelperPtr& ptr )
{
    self = ptr;
}

