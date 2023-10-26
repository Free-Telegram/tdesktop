/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/controls/history_view_webpage_processor.h"

#include "base/unixtime.h"
#include "data/data_chat_participant_status.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "data/data_web_page.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"

namespace HistoryView::Controls {

WebPageText TitleAndDescriptionFromWebPage(not_null<WebPageData*> d) {
	QString resultTitle, resultDescription;
	const auto document = d->document;
	const auto author = d->author;
	const auto siteName = d->siteName;
	const auto title = d->title;
	const auto description = d->description;
	const auto filenameOrUrl = [&] {
		return ((document && !document->filename().isEmpty())
			? document->filename()
			: d->url);
	};
	const auto authorOrFilename = [&] {
		return (author.isEmpty()
			? filenameOrUrl()
			: author);
	};
	const auto descriptionOrAuthor = [&] {
		return (description.text.isEmpty()
			? authorOrFilename()
			: description.text);
	};
	if (siteName.isEmpty()) {
		if (title.isEmpty()) {
			if (description.text.isEmpty()) {
				resultTitle = author;
				resultDescription = filenameOrUrl();
			} else {
				resultTitle = description.text;
				resultDescription = authorOrFilename();
			}
		} else {
			resultTitle = title;
			resultDescription = descriptionOrAuthor();
		}
	} else {
		resultTitle = siteName;
		resultDescription = title.isEmpty()
			? descriptionOrAuthor()
			: title;
	}
	return { resultTitle, resultDescription };
}

bool DrawWebPageDataPreview(
		QPainter &p,
		not_null<WebPageData*> webpage,
		not_null<PeerData*> context,
		QRect to) {
	const auto document = webpage->document;
	const auto photo = webpage->photo;
	if ((!photo || photo->isNull())
		&& (!document
			|| !document->hasThumbnail()
			|| document->isPatternWallPaper())) {
		return false;
	}

	const auto preview = photo
		? photo->getReplyPreview(Data::FileOrigin(), context, false)
		: document->getReplyPreview(Data::FileOrigin(), context, false);
	if (preview) {
		const auto w = preview->width();
		const auto h = preview->height();
		if (w == h) {
			p.drawPixmap(to.x(), to.y(), preview->pix());
		} else {
			const auto from = (w > h)
				? QRect((w - h) / 2, 0, h, h)
				: QRect(0, (h - w) / 2, w, w);
			p.drawPixmap(to, preview->pix(), from);
		}
	}
	return true;
}

[[nodiscard]] bool ShowWebPagePreview(WebPageData *page) {
	return page && !page->failed;
}

WebPageText ProcessWebPageData(WebPageData *page) {
	auto previewText = TitleAndDescriptionFromWebPage(page);
	if (previewText.title.isEmpty()) {
		if (page->document) {
			previewText.title = tr::lng_attach_file(tr::now);
		} else if (page->photo) {
			previewText.title = tr::lng_attach_photo(tr::now);
		}
	}
	return previewText;
}

WebpageProcessor::WebpageProcessor(
	not_null<History*> history,
	not_null<Ui::InputField*> field)
: _history(history)
, _api(&history->session().mtp())
, _parser(field)
, _timer([=] {
	if (!ShowWebPagePreview(_data) || _link.isEmpty()) {
		return;
	}
	request();
}) {
	_history->session().downloaderTaskFinished(
	) | rpl::filter([=] {
		return _data && (_data->document || _data->photo);
	}) | rpl::start_with_next([=] {
		_repaintRequests.fire({});
	}, _lifetime);

	_history->owner().webPageUpdates(
	) | rpl::filter([=](not_null<WebPageData*> page) {
		return (_data == page.get());
	}) | rpl::start_with_next([=] {
		updateFromData();
	}, _lifetime);

	_parser.list().changes(
	) | rpl::start_with_next([=](QStringList &&parsed) {
		_parsedLinks = std::move(parsed);
		checkPreview();
	}, _lifetime);
}

rpl::producer<> WebpageProcessor::repaintRequests() const {
	return _repaintRequests.events();
}

Data::WebPageDraft WebpageProcessor::draft() const {
	return _draft;
}

void WebpageProcessor::apply(Data::WebPageDraft draft, bool reparse) {
	_api.request(base::take(_requestId)).cancel();
	if (draft.removed) {
		_draft = draft;
		if (_parsedLinks.empty()) {
			_draft.removed = false;
		}
		_data = nullptr;
		_links = QStringList();
		_link = QString();
		_parsed = WebpageParsed();
		updateFromData();
	} else if (draft.manual && draft.id && !draft.url.isEmpty()) {
		_draft = draft;
		_parsedLinks = QStringList();
		_links = QStringList();
		_link = _draft.url;
		const auto page = _history->owner().webpage(draft.id);
		if (page->url == draft.url) {
			_data = page;
			updateFromData();
		} else {
			request();
		}
	} else if (!draft.manual && !_draft.manual) {
		_draft = draft;
		checkNow(reparse);
	}
}

void WebpageProcessor::updateFromData() {
	_timer.cancel();
	auto parsed = WebpageParsed();
	if (ShowWebPagePreview(_data)) {
		if (const auto till = _data->pendingTill) {
			parsed.drawPreview = [](QPainter &p, QRect to) {
				return false;
			};
			parsed.title = tr::lng_preview_loading(tr::now);
			parsed.description = _link;

			const auto timeout = till - base::unixtime::now();
			_timer.callOnce(
				std::max(timeout, 0) * crl::time(1000));
		} else {
			const auto webpage = _data;
			const auto context = _history->peer;
			const auto preview = ProcessWebPageData(_data);
			parsed.title = preview.title;
			parsed.description = preview.description;
			parsed.drawPreview = [=](QPainter &p, QRect to) {
				return DrawWebPageDataPreview(p, webpage, context, to);
			};
		}
	}
	_parsed = std::move(parsed);
	_repaintRequests.fire({});
}

void WebpageProcessor::request() {
	const auto link = _link;
	const auto done = [=](const MTPDmessageMediaWebPage &data) {
		const auto page = _history->owner().processWebpage(data.vwebpage());
		if (page->pendingTill > 0
			&& page->pendingTill < base::unixtime::now()) {
			page->pendingTill = 0;
			page->failed = true;
		}
		_cache.emplace(link, page->failed ? nullptr : page.get());
		if (_link == link && !_draft.removed && !_draft.manual) {
			_data = (page->id && !page->failed)
				? page.get()
				: nullptr;
			_draft.id = page->id;
			_draft.url = page->url;
			updateFromData();
		}
	};
	const auto fail = [=] {
		_cache.emplace(link, nullptr);
		if (_link == link && !_draft.removed && !_draft.manual) {
			_links = QStringList();
			checkPreview();
		}
	};
	_requestId = _api.request(
		MTPmessages_GetWebPagePreview(
			MTP_flags(0),
			MTP_string(_link),
			MTPVector<MTPMessageEntity>()
	)).done([=](const MTPMessageMedia &result, mtpRequestId requestId) {
		if (_requestId == requestId) {
			_requestId = 0;
		}
		result.match([=](const MTPDmessageMediaWebPage &data) {
			done(data);
		}, [&](const auto &d) {
			fail();
		});
	}).fail([=](const MTP::Error &error, mtpRequestId requestId) {
		if (_requestId == requestId) {
			_requestId = 0;
		}
		fail();
	}).send();
}

void WebpageProcessor::checkNow(bool force) {
	_parser.parseNow();
	if (force) {
		_link = QString();
		_links = QStringList();
		if (_parsedLinks.isEmpty()) {
			_data = nullptr;
			updateFromData();
			return;
		}
	}
	checkPreview();
}

void WebpageProcessor::checkPreview() {
	const auto previewRestricted = _history->peer
		&& _history->peer->amRestricted(ChatRestriction::EmbedLinks);
	if (_parsedLinks.empty()) {
		_draft.removed = false;
	}
	if (_draft.removed) {
		return;
	} else if (previewRestricted) {
		apply({ .removed = true });
		_draft.removed = false;
		return;
	} else if (_draft.manual) {
		return;
	} else if (_links == _parsedLinks) {
		return;
	}
	_links = _parsedLinks;

	auto page = (WebPageData*)nullptr;
	auto chosen = QString();
	for (const auto &link : _links) {
		const auto i = _cache.find(link);
		if (i == end(_cache)) {
			chosen = link;
			break;
		} else if (i->second) {
			if (i->second->failed) {
				i->second = nullptr;
			} else {
				chosen = link;
				page = i->second;
				break;
			}
		}
	}
	if (_link != chosen) {
		_link = chosen;
		_api.request(base::take(_requestId)).cancel();
		if (!page && !_link.isEmpty()) {
			request();
		}
	}
	if (page) {
		_data = page;
		_draft.id = _data->id;
		_draft.url = _data->url;
	} else {
		_data = nullptr;
		_draft = {};
	}
	updateFromData();
}

rpl::producer<WebpageParsed> WebpageProcessor::parsedValue() const {
	return _parsed.value();
}

} // namespace HistoryView::Controls