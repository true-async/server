<?php

/**
 * @generate-class-entries
 */

namespace TrueAsync;

/**
 * Content-Disposition for HttpResponse::sendFile().
 */
enum SendFileDisposition: string
{
    case INLINE     = 'inline';
    case ATTACHMENT = 'attachment';
}

/**
 * Options for HttpResponse::sendFile(). Value object, immutable.
 *
 * @strict-properties
 * @not-serializable
 */
final readonly class SendFileOptions
{
    public function __construct(
        public ?string             $contentType     = null,
        public SendFileDisposition $disposition     = SendFileDisposition::INLINE,
        public ?string             $downloadName    = null,
        public ?string             $cacheControl    = null,
        public bool                $etag            = true,
        public bool                $lastModified    = true,
        public bool                $acceptRanges    = true,
        public bool                $precompressed   = true,
        public bool                $conditional     = true,
        public bool                $deleteAfterSend = false,
        public ?int                $status          = null,
    ) {}
}
