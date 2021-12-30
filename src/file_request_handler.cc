/*MT*

    MediaTomb - http://www.mediatomb.cc/

    file_request_handler.cc - this file is part of MediaTomb.

    Copyright (C) 2005 Gena Batyan <bgeradz@mediatomb.cc>,
                       Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>

    Copyright (C) 2006-2010 Gena Batyan <bgeradz@mediatomb.cc>,
                            Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>,
                            Leonhard Wimmer <leo@mediatomb.cc>

    MediaTomb is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation.

    MediaTomb is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    version 2 along with MediaTomb; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.

    $Id$
*/

/// \file file_request_handler.cc

#include "file_request_handler.h" // API

#include <sys/stat.h>
#include <unistd.h>

#include "config/config_manager.h"
#include "content/content_manager.h"
#include "database/database.h"
#include "iohandler/file_io_handler.h"
#include "metadata/metadata_handler.h"
#include "transcoding/transcode_dispatcher.h"
#include "util/tools.h"
#include "util/upnp_headers.h"
#include "util/upnp_quirks.h"
#include "web/session_manager.h"

FileRequestHandler::FileRequestHandler(const std::shared_ptr<ContentManager>& content, std::shared_ptr<UpnpXMLBuilder> xmlBuilder)
    : RequestHandler(content)
    , xmlBuilder(std::move(xmlBuilder))
{
}

void FileRequestHandler::getInfo(const char* filename, UpnpFileInfo* info)
{
    log_debug("start: {}", filename);

    auto quirks = getQuirks(info);

    auto params = parseParameters(filename, LINK_FILE_REQUEST_HANDLER);
    auto obj = loadObject(params);

    auto [resourceHandler, resourceId] = parseResourceInfo(params);

    // If its not an Item and there is no handler, we cant service this request.
    if (!obj->isItem() && resourceHandler.empty()) {
        throw_std_runtime_error("Requested object {} is not an item", filename);
    }

    auto item = std::dynamic_pointer_cast<CdsItem>(obj);

    // Why text?
    std::string mimeType = !resourceHandler.empty() ? MIMETYPE_TEXT : "";

    fs::path path;
    bool isResourceFile = false;
    if (!resourceHandler.empty()) {
        auto resPath = obj->getResource(resourceId)->getAttribute(R_RESOURCE_FILE);
        isResourceFile = !resPath.empty();
        if (isResourceFile) {
            path = resPath;
        }
    } else {
        path = item->getLocation();
    }

    // Check the path (if its real) is accessible
    struct stat statbuf {
    };
    int ret = stat(path.c_str(), &statbuf);
    if (ret != 0) {
        if (isResourceFile) {
            throw SubtitlesNotFoundException(fmt::format("Subtitle file {} is not available.", path.c_str()));
        }
        throw_std_runtime_error("Failed to open {}: {}", path.c_str(), std::strerror(errno));
    }

    // If we get to here we can read the thing
    UpnpFileInfo_set_IsReadable(info, true);
    UpnpFileInfo_set_LastModified(info, statbuf.st_mtime);
    UpnpFileInfo_set_IsDirectory(info, S_ISDIR(statbuf.st_mode));

    // for transcoded resources res_id will always be negative
    std::string trProfile = getValueOrDefault(params, URL_PARAM_TRANSCODE_PROFILE_NAME);

    auto headers = std::make_unique<Headers>();

    // some resources are created dynamically and not saved in the database,
    // so we can not load such a resource for a particular item, we will have
    // to trust the resource handler parameter
    if (!resourceHandler.empty()) {
        if (resourceId == CH_DEFAULT || resourceId >= obj->getResourceCount()) {
            throw_std_runtime_error("Requested resource not found");
        }

        auto resource = obj->getResource(resourceId);
        auto metadataHandler = getResourceMetadataHandler(obj, resourceHandler, resource);

        std::string protocolInfo = getValueOrDefault(resource->getAttributes(), "protocolInfo");
        if (!protocolInfo.empty()) {
            mimeType = getMTFromProtocolInfo(protocolInfo);
        }
        if (mimeType.empty())
            mimeType = metadataHandler->getMimeType();

        // Why doesnt serveContent take an actual Resource object....
        auto ioHandler = metadataHandler->serveContent(obj, resourceId);

        // Get size
        ioHandler->open(UPNP_READ);
        ioHandler->seek(0L, SEEK_END);
        off_t size = ioHandler->tell();
        ioHandler->close();
        UpnpFileInfo_set_FileLength(info, size);

    } else if (!isResourceFile && !trProfile.empty()) {

        auto transcodingProfile = config->getTranscodingProfileListOption(CFG_TRANSCODING_PROFILE_LIST)->getByName(trProfile);
        if (!transcodingProfile)
            throw_std_runtime_error("Transcoding of file {} but no profile matching the name {} found", path.c_str(), trProfile);

        mimeType = transcodingProfile->getTargetMimeType();

        auto mappings = config->getDictionaryOption(CFG_IMPORT_MAPPINGS_MIMETYPE_TO_CONTENTTYPE_LIST);
        if (getValueOrDefault(mappings, mimeType) == CONTENT_TYPE_PCM) {
            std::string freq = obj->getResource(0)->getAttribute(R_SAMPLEFREQUENCY);
            std::string nrch = obj->getResource(0)->getAttribute(R_NRAUDIOCHANNELS);
            if (!freq.empty())
                mimeType += fmt::format(";rate={}", freq);
            if (!nrch.empty())
                mimeType += fmt::format(";channels={}", nrch);
        }

#ifdef UPNP_USING_CHUNKED
        UpnpFileInfo_set_FileLength(info, UPNP_USING_CHUNKED);
#else
        UpnpFileInfo_set_FileLength(info, -1);
#endif
    } else if (item) {
        UpnpFileInfo_set_FileLength(info, statbuf.st_size);

        quirks->addCaptionInfo(item, headers);

        auto mappings = config->getDictionaryOption(CFG_IMPORT_MAPPINGS_MIMETYPE_TO_CONTENTTYPE_LIST);
        auto resource = item->getResource(0);
        std::string dlnaContentHeader = getDLNAContentHeader(config, getValueOrDefault(mappings, item->getMimeType()), resource ? resource->getAttribute(R_VIDEOCODEC) : "", resource ? resource->getAttribute(R_AUDIOCODEC) : "");
        if (!dlnaContentHeader.empty()) {
            headers->addHeader(UPNP_DLNA_CONTENT_FEATURES_HEADER, dlnaContentHeader);
        }
    }

    if (mimeType.empty() && item)
        mimeType = item->getMimeType();

    std::string dlnaTransferHeader = getDLNATransferHeader(config, mimeType);
    if (!dlnaTransferHeader.empty()) {
        headers->addHeader(UPNP_DLNA_TRANSFER_MODE_HEADER, dlnaTransferHeader);
    }

#ifdef USING_NPUPNP
    info->content_type = std::move(mimeType);
#else
    UpnpFileInfo_set_ContentType(info, mimeType.c_str());
#endif

    headers->writeHeaders(info);

    // log_debug("getInfo: Requested {}, ObjectID: {}, Location: {}, MimeType: {}",
    //      filename, object_id.c_str(), path.c_str(), info->content_type);

    log_debug("end: {}", filename);
}

std::unique_ptr<MetadataHandler> FileRequestHandler::getResourceMetadataHandler(const std::shared_ptr<CdsObject>& obj, const std::string& resourceHandler, const std::shared_ptr<CdsResource>& resource) const
{
    int resHandler = (!resourceHandler.empty()) ? stoiString(resourceHandler) : resource->getHandlerType();
    return MetadataHandler::createHandler(context, resHandler);
}

std::unique_ptr<IOHandler> FileRequestHandler::open(const char* filename, enum UpnpOpenFileMode mode)
{
    log_debug("start: {}", filename);

    // We explicitly do not support UPNP_WRITE due to security reasons.
    if (mode != UPNP_READ) {
        throw_std_runtime_error("UPNP_WRITE unsupported");
    }

    auto params = parseParameters(filename, LINK_FILE_REQUEST_HANDLER);
    auto obj = loadObject(params);
    auto [resourceHandler, resourceId] = parseResourceInfo(params);

    // Serve metadata resources
    if (!resourceHandler.empty()) {
        auto resource = obj->getResource(resourceId);
        auto metadataHandler = getResourceMetadataHandler(obj, resourceHandler, resource);
        return metadataHandler->serveContent(obj, resourceId);
    }

    auto item = std::dynamic_pointer_cast<CdsItem>(obj);
    auto path = item->getLocation();

    content->triggerPlayHook(obj);

    // Transcoding
    std::string trProfile = getValueOrDefault(params, URL_PARAM_TRANSCODE_PROFILE_NAME);
    if (!trProfile.empty()) {
        auto transcodingProfile = config->getTranscodingProfileListOption(CFG_TRANSCODING_PROFILE_LIST)->getByName(trProfile);
        if (!transcodingProfile)
            throw_std_runtime_error("Transcoding of file {} but no profile matching the name {} found", path.c_str(), trProfile);

        std::string range = getValueOrDefault(params, "range");

        auto transcodeDispatcher = std::make_unique<TranscodeDispatcher>(content);
        return transcodeDispatcher->serveContent(transcodingProfile, path, item, range);
    }

    // Boring old file
    return std::make_unique<FileIOHandler>(path);
}

std::pair<std::string, std::size_t> FileRequestHandler::parseResourceInfo(std::map<std::string, std::string>& params) const
{
    auto resourceHandler = getValueOrDefault(params, RESOURCE_HANDLER);

    std::size_t resourceId = 0;
    try {
        auto resIdParam = params.at(URL_RESOURCE_ID);
        if (resIdParam != URL_VALUE_TRANSCODE_NO_RES_ID) {
            resourceId = stoiString(resIdParam);
        }
    } catch (const std::out_of_range&) {
    }

    log_debug("Resource Handler: '{}'", resourceHandler);
    log_debug("Resource ID: {}", resourceId);

    return std::make_pair<std::string, std::size_t>(std::move(resourceHandler), std::move(resourceId));
}

std::unique_ptr<Quirks> FileRequestHandler::getQuirks(const UpnpFileInfo* info) const
{
    const struct sockaddr_storage* ctrlPtIPAddr = UpnpFileInfo_get_CtrlPtIPAddr(info);
    // HINT: most clients do not report exactly the same User-Agent for UPnP services and file request.
    std::string userAgent = UpnpFileInfo_get_Os_cstr(info);
    return std::make_unique<Quirks>(context, ctrlPtIPAddr, std::move(userAgent));
}
