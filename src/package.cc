#include "package.hh"

#include "worker.hh"
#include "sforeader.hh"

#include <sha256.h>
#include <pkg.h>
#include <psvimg-create.h>
#include <miniz_zip.h>

#include <QDir>
#include <QDebug>
#include <QSettings>

const char *HENCORE_FULL_FILE = "h-encore-full.zip";
const char *HENCORE_FULL_SHA256 = "3ea59bdf6e7d8f5aa96cabd4f0577fcf78822970f463680b0758a5aaa332452d";

const char *BSPKG_URL = "http://ares.dl.playstation.net/cdn/JP0741/PCSG90096_00/xGMrXOkORxWRyqzLMihZPqsXAbAXLzvAdJFqtPJLAZTgOcqJobxQAhLNbgiFydVlcmVOrpZKklOYxizQCRpiLfjeROuWivGXfwgkq.pkg";
const char *BSPKG_FILE = "BitterSmile.pkg";
const char *BSPKG_SHA256 = "280a734a0b40eedac2b3aad36d506cd4ab1a38cd069407e514387a49d81b9302";

Package::Package(const QString &basePath, const QString &appPath, QObject *obj_parent): QObject(obj_parent), downloader(obj_parent) {
    pkgBasePath = basePath;
    appBasePath = appPath;
    genExtraUnpackList();
    connect(&downloader, SIGNAL(finishedFile(QFile*)), this, SLOT(downloadFinished(QFile*)));
    connect(&downloader, SIGNAL(finishedGet(void*)), this, SLOT(fetchFinished(void*)));
    connect(&downloader, SIGNAL(downloadProgress(uint64_t, uint64_t)), this, SLOT(downloadProg(uint64_t, uint64_t)));
    connect(this, SIGNAL(startDownload()), SLOT(checkHencoreFull()));
    connect(this, SIGNAL(noHencoreFull()), SLOT(downloadDemo()));
    connect(this, SIGNAL(unpackedDemo()), SLOT(startUnpackZips()));
    connect(this, SIGNAL(unpackedZip(QString)), SLOT(createPsvImgs(QString)));
    connect(this, SIGNAL(unpackNext()), SLOT(doUnpackZip()));
}

Package::~Package() {
}

void Package::tips() {
    if (accountId.isEmpty())
        emit setStatusText(tr("Launch Content Manager on PS Vita and connect to computer."));
}

void Package::selectExtraApp(const QString &titleId, bool select) {
    if (select) {
        auto ite = extraApps.find(titleId);
        if (ite != extraApps.end())
            selectedExtraApps[titleId] = &ite.value();
    } else {
        selectedExtraApps.remove(titleId);
    }
}

void Package::get(const QString &url, QString &result) {
    downloader.doGet(url, &result);
}

void Package::download(const QString &url, const QString &localFilename, const char *sha256sum) {
    QDir dir(pkgBasePath);
    QString filename = dir.filePath(localFilename);
    if (QFile::exists(filename)) {
        static int n = 0;
        Worker::start(this, [this, filename, sha256sum](void *arg) {
            *(int*)arg = 0;
            if (verify(filename, sha256sum)) {
                *(int*)arg = 1;
                return;
            }
            qDebug("Removing old file.");
            bool succ = false;
            for (int i = 0; i < 5; ++i) {
                if (QFile::remove(filename)) {
                    succ = true;
                    break;
                }
                for (int j = 0; j < 20; ++j) {
                    QThread::msleep(100);
                }
            }
            if (!succ) {
                qDebug("Failed to remove old file, operation aborted.");
                *(int*)arg = 2;
                return;
            }
        }, [this, url, filename](void *arg) {
            switch (*(int*)arg) {
            case 0:
                startDownload(url, filename);
                break;
            case 1: {
                QFileInfo fi(filename);
                if (fi.fileName() == BSPKG_FILE)
                    startUnpackDemo(BSPKG_FILE);
                break;
            }
            }
        }, &n);
    } else {
        startDownload(url, filename);
    }
}

void Package::startDownload(const QString &url, const QString & localFilename) {
    qDebug("Start downloading from %s...", url.toUtf8().constData());
    emit setStatusText(tr("Downloading %1").arg(QFileInfo(localFilename).fileName()));
    QFile *f = new QFile(localFilename);
    f->open(QFile::WriteOnly | QFile::Truncate);
    downloader.doDownload(url, f);
}

static uint64_t demoSize = 0;

void _output_progress_init(void *arg, uint64_t size) {
    demoSize = size;
}

void _output_progress(void *arg, uint64_t progress) {
    emit ((Package*)arg)->setPercent((int)(progress * 100ULL / demoSize));
}

void Package::startUnpackDemo(const char *filename) {
    static bool succ = false;
    Worker::start(this, [this, filename](void *arg) {
        *(bool*)arg = false;
        qDebug("Unpacking %s...", filename);
        emit setStatusText(tr("Unpacking %1").arg(filename));
        pkg_disable_output();
        pkg_set_func(nullptr, nullptr, _output_progress_init, _output_progress, this);
        QString oldDir = QDir::currentPath();
        QDir curr(pkgBasePath);
        if (curr.cd("h-encore")) {
            curr.removeRecursively();
            curr.cdUp();
        }
        curr.mkpath("h-encore");
        QDir::setCurrent(pkgBasePath);
        int res = pkg_dec(filename, "h-encore/", nullptr);
        if (res < 0) {
            qWarning("Failed to unpack %s!", filename);
            emit setStatusText(tr("Failed to unpack %1").arg(filename));
        } else {
            curr.cd("h-encore");
            curr.cd("app");
            curr.rename("PCSG90096", "ux0_temp_game_PCSG90096_app_PCSG90096");
            *(bool*)arg = true;
        }
        QDir::setCurrent(oldDir);
    }, [this](void *arg) {
        if (*(bool*)arg) {
            qDebug("Done.");
            emit setPercent(100);
            emit unpackedDemo();
        }
    }, &succ);
}

void Package::doUnpackZip() {
    if (unzipQueue.isEmpty()) return;
    auto p = unzipQueue.front();
    auto filename = p.fullPath;
    auto titleID = p.titleId;
    unzipQueue.pop_front();
    static bool succ = false;
    Worker::start(this, [this, filename, titleID](void *arg) {
        emit setPercent(0);
        bool isHencore = titleID == "PCSG90096";
        *(bool*)arg = false;
        qDebug("Decompressing %s...", filename.toUtf8().constData());
        emit setStatusText(tr("Decompressing %1").arg(filename));
        QDir dir(pkgBasePath);
        if (!isHencore) {
            dir.cdUp();
            dir.mkpath("extra");
            dir.cd("extra");
        }
        QFile file(filename);
        file.open(QFile::ReadOnly);
        mz_zip_archive arc;
        mz_zip_zero_struct(&arc);
        arc.m_pIO_opaque = &file;
        arc.m_pRead = [](void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n)->size_t {
            QFile *f = (QFile*)pOpaque;
            if (!f->seek(file_ofs)) return 0;
            return f->read((char*)pBuf, n);
        };
        if (!mz_zip_reader_init(&arc, file.size(), 0)) {
            file.close();
            qWarning("Unable to decompress %s!", filename.toUtf8().constData());
            emit setStatusText(tr("Failed to decompress %1").arg(filename));
            unzipQueue.clear();
            return;
        }
        uint32_t num = mz_zip_reader_get_num_files(&arc);
        for (uint32_t i = 0; i < num; ++i) {
            emit setPercent(i * 100 / num);
            char zfilename[1024];
            mz_zip_reader_get_filename(&arc, i, zfilename, 1024);
            if (mz_zip_reader_is_file_a_directory(&arc, i)) {
                dir.mkpath(zfilename);
            } else {
                QFile wfile(dir.filePath(zfilename));
                wfile.open(QFile::WriteOnly | QFile::Truncate);
                mz_zip_reader_extract_to_callback(&arc, i,
                    [](void *pOpaque, mz_uint64 file_ofs, const void *pBuf, size_t n)->size_t {
                    QFile *f = (QFile*)pOpaque;
                    return f->write((const char*)pBuf, n);
                }, &wfile, 0);
                wfile.close();
            }
        }
        mz_zip_reader_end(&arc);
        file.close();
        if (isHencore) {
            QFile::copy(pkgBasePath + QString("/h-encore/app/ux0_temp_game_%1_app_%1/sce_sys/package/temp.bin").arg(titleID),
                pkgBasePath + QString("/h-encore/license/ux0_temp_game_%1_license_app_%1/6488b73b912a753a492e2714e9b38bc7.rif").arg(titleID));
        }
        emit setPercent(100);
        *(bool*)arg = true;
    }, [this, titleID](void *arg) {
        if (*(bool*)arg) {
            qDebug("Done.");
            emit unpackedZip(titleID);
        }
    }, &succ);
}

void Package::genExtraUnpackList() {
    /* search zips in app dir */
    QDir dir(appBasePath);
    QFileInfoList qsl = dir.entryInfoList({ "*.zip" }, QDir::Files, QDir::Name);
    QString titleId;
    QString name;
    foreach(const QFileInfo &info, qsl) {
        QString fullPath = dir.filePath(info.fileName());
        if (checkValidAppZip(fullPath, titleId, name))
            extraApps[titleId] = AppInfo{ fullPath, titleId, name };
    }
    dir = pkgBasePath;
    if (!dir.cdUp() || !dir.cd("extra")) return;
    /* remove old unzipped dirs */
    qsl = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    foreach(const QFileInfo &info, qsl) {
        if (dir.cd(info.fileName())) {
            dir.removeRecursively();
            dir.cdUp();
        }
    }
    /* search zips in `extra' */
    qsl = dir.entryInfoList({"*.zip"}, QDir::Files, QDir::Name);
    foreach(const QFileInfo &info, qsl) {
        QString fullPath = dir.filePath(info.fileName());
        if (checkValidAppZip(fullPath, titleId, name))
            extraApps[titleId] = AppInfo{ fullPath, titleId, name };
    }
}

void Package::startUnpackZipsFull() {
    unzipQueue.clear();
    for (auto &p : selectedExtraApps) {
        unzipQueue.push_back(*p);
    }
    unzipQueue.push_back(AppInfo{ QDir(pkgBasePath).filePath(HENCORE_FULL_FILE), "PCSG90096", "h-encore" });
    emit unpackNext();
}

void Package::startUnpackZips() {
    unzipQueue.clear();
    for (auto &p : selectedExtraApps) {
        unzipQueue.push_back(*p);
    }
    unzipQueue.push_back(AppInfo{ ":/main/resources/raw/h-encore.zip", "PCSG90096", "h-encore" });
    emit unpackNext();
}

bool Package::verify(const QString &filepath, const char *sha256sum) {
    QFile file(filepath);
    if (file.open(QFile::ReadOnly)) {
        qDebug("Verifying sha256sum for %s...", filepath.toUtf8().constData());
        emit setStatusText(tr("Verifying %1").arg(QFileInfo(filepath).fileName()));
        sha256_context ctx;
        sha256_init(&ctx);
        sha256_starts(&ctx);
        size_t bufsize = 2 * 1024 * 1024;
        char *buf;
        do {
            bufsize >>= 1;
            buf = new char[bufsize];
        } while (buf == nullptr);
        qint64 fsz = file.size();
        qint64 curr = 0;
        qint64 rsz;
        emit setPercent(0);
        while ((rsz = file.read(buf, bufsize)) > 0) {
            sha256_update(&ctx, (const uint8_t*)buf, rsz);
            curr += rsz;
            emit setPercent(curr * 100LL / fsz);
        }
        delete[] buf;
        file.close();
        uint8_t sum[32];
        sha256_final(&ctx, sum);
        QByteArray array((const char*)sum, 32);
        if (QString(array.toHex()) == sha256sum) {
            qDebug("sha256sum correct.");
            return true;
        } else {
            qWarning("sha256sum mismatch.");
            emit setStatusText(tr("sha256sum mismatch! Please check your network."));
        }
    }
    return false;
}

void Package::createPsvImgSingleDir(const QString &titleID, const char *singleDir) {
    QDir curr = QDir::current();
    if (curr.exists(singleDir)) {
        psvimg_create(singleDir, (titleID + "/" + singleDir).toUtf8().constData(), backupKey.toUtf8().constData(), singleDir, 0);
        curr.cd(singleDir);
        curr.removeRecursively();
    }
}

bool Package::checkValidAppZip(const QString & filepath, QString &titleId, QString &name) {
    QFile file(filepath);
    file.open(QFile::ReadOnly);
    mz_zip_archive arc;
    mz_zip_zero_struct(&arc);
    arc.m_pIO_opaque = &file;
    arc.m_pRead = [](void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n)->size_t {
        QFile *f = (QFile*)pOpaque;
        if (!f->seek(file_ofs)) return 0;
        return f->read((char*)pBuf, n);
    };
    if (!mz_zip_reader_init(&arc, file.size(), 0)) {
        file.close();
        return false;
    }
    uint32_t num = mz_zip_reader_get_num_files(&arc);
    bool hasApp = false, hasAppMeta = false, hasTitle = false;
    for (uint32_t i = 0; i < num; ++i) {
        char zfilename[1024];
        mz_zip_reader_get_filename(&arc, i, zfilename, 1024);
        if (!mz_zip_reader_is_file_a_directory(&arc, i)) continue;
        QString dirName = QString(zfilename);
        if (dirName == "app/") {
            hasApp = true;
        } else if (dirName == "appmeta/") {
            hasAppMeta = true;
        } else if (dirName.length() == 10 && dirName.indexOf("/") == 9) {
            titleId = dirName.left(9);
            hasTitle = true;
        }
        if (hasApp && hasAppMeta && hasTitle) break;
    }
    int index = mz_zip_reader_locate_file(&arc, (titleId + "/sce_sys/param.sfo").toUtf8().constData(), NULL, 0);
    if (index < 0) {
        mz_zip_reader_end(&arc);
        return false;
    }
    QByteArray data;
    mz_zip_archive_file_stat stat;
    mz_zip_reader_file_stat(&arc, index, &stat);
    data.resize(stat.m_uncomp_size);
    if (mz_zip_reader_extract_to_mem(&arc, index, data.data(), stat.m_uncomp_size, 0) == MZ_TRUE) {
        SfoReader reader;
        if (reader.load(data)) {
            name = reader.value("TITLE", titleId.toUtf8().constData());
        }
    }
    mz_zip_reader_end(&arc);
    return hasApp && hasAppMeta && hasTitle;
}

void Package::getBackupKey(const QString &aid) {
    if (accountId != aid) {
        accountId = aid;
        QSettings settings;
        settings.beginGroup("BackupKeys");
        backupKey = settings.value(accountId).toString();
        settings.endGroup();
        if (backupKey.isEmpty()) {
            get(QString("http://cma.henkaku.xyz/?aid=%1").arg(aid), backupKey);
            qDebug("Fetching backup key from cma.henkaku.xyz...");
            emit setStatusText(tr("Fetching backup key from cma.henkaku.xyz"));
        } else {
            emit setStatusText(tr("Fetched backup key.\nClick button to START!"));
            emit gotBackupKey();
        }
    }
}

void Package::finishBuildData() {
    emit setPercent(100);
    emit setStatusText(tr("Everything is ready, now follow below steps on your PS Vita:\n"
    "1. Launch Content Manager and connect to your computer.\n"
    "2. Select \"PC -> PS Vita System\" -> \"Applications\" -> \"PS Vita\".\n"
    "3. Transfer \"h-encore\" to your PS Vita.\n"
    "4. Run \"h-encore\" and... Yay, that's it!"));
}

void Package::checkHencoreFull() {
    static bool succ = false;
    Worker::start(this, [this](void *arg) {
        QDir dir(pkgBasePath);
        QString filename = dir.filePath(HENCORE_FULL_FILE);
        *(bool*)arg = verify(filename, HENCORE_FULL_SHA256);
    }, [this](void *arg) {
        if (*(bool*)arg) {
            startUnpackZipsFull();
        } else {
            emit noHencoreFull();
        }
    }, &succ);
}

void Package::downloadDemo() {
    download(BSPKG_URL, BSPKG_FILE, BSPKG_SHA256);
}

void Package::createPsvImgs(QString titleID) {
    Worker::start(this, [this, titleID](void*) {
        bool isHencore = titleID == "PCSG90096";
        QString oldDir = QDir::currentPath();
        QDir curr(pkgBasePath);
        if (isHencore) {
            curr.cd("h-encore");
            QDir::setCurrent(curr.path());
            if (trimApp) {
                qDebug("Trimming package...");
                emit setStatusText(tr("Trimming package"));
                if (curr.cd("app") && curr.cd("ux0_temp_game_PCSG90096_app_PCSG90096")) {
                    QDir cdir = curr;
                    if (cdir.cd("resource") && cdir.cd("movie"))
                        cdir.removeRecursively();
                    cdir = curr;
                    if (cdir.cd("resource") && cdir.cd("sound"))
                        cdir.removeRecursively();
                    cdir = curr;
                    if (cdir.cd("resource") && cdir.cd("text") && cdir.cd("01"))
                        cdir.removeRecursively();
                    cdir = curr;
                    if (cdir.cd("resource") && cdir.cd("image") && cdir.cd("bg"))
                        cdir.removeRecursively();
                    cdir = curr;
                    if (cdir.cd("resource") && cdir.cd("image") && cdir.cd("ev"))
                        cdir.removeRecursively();
                    cdir = curr;
                    if (cdir.cd("resource") && cdir.cd("image") && cdir.cd("icon"))
                        cdir.removeRecursively();
                    cdir = curr;
                    if (cdir.cd("resource") && cdir.cd("image") && cdir.cd("stitle"))
                        cdir.removeRecursively();
                    cdir = curr;
                    if (cdir.cd("resource") && cdir.cd("image") && cdir.cd("tachie"))
                        cdir.removeRecursively();
                }
                qDebug("Done trimming.");
            }
        } else {
            curr.cdUp();
            curr.cd("extra");
            QDir::setCurrent(curr.path());
        }
        qDebug("Creating psvimg's for %s...", titleID.toUtf8().constData());
        emit setStatusText(tr("Createing psvimg's"));
        emit setPercent(0);
        createPsvImgSingleDir(titleID, "app");
        emit setPercent(90);
        createPsvImgSingleDir(titleID, "appmeta");
        emit setPercent(93);
        createPsvImgSingleDir(titleID, "license");
        emit setPercent(96);
        createPsvImgSingleDir(titleID, "savedata");
        emit setPercent(99);
        QDir::setCurrent(oldDir);
    }, [this, titleID](void*) {
        qDebug("Done.");
        if (unzipQueue.isEmpty())
            emit createdPsvImgs();
        else
            emit unpackNext();
    });
}

void Package::downloadProg(uint64_t curr, uint64_t total) {
    if (total > 0)
        emit setPercent((int)(curr * 100ULL / total));
}

void Package::fetchFinished(void *arg) {
    QString *str = (QString*)arg;
    int index = str->indexOf("</b>: ");
    if (index < 0) {
        str->clear();
        qWarning("Cannot get backup key from your AID");
        emit setStatusText(tr("Cannot get backup key from your AID.\nPlease check your network connection!"));
        return;
    }
    emit setStatusText(tr("Fetched backup key.\nClick button to START!"));
    qDebug("Done.");
    *str = str->mid(index + 6, 64);
    QSettings settings;
    settings.beginGroup("BackupKeys");
    settings.setValue(accountId, *str);
    settings.endGroup();
    emit gotBackupKey();
}

void Package::downloadFinished(QFile *f) {
    QString filepath = f->fileName();
    QFileInfo fi(filepath);
    delete f;
    static int n = 0;
    if (fi.fileName() == BSPKG_FILE) {
        Worker::start(this, [this, filepath](void *arg) {
            if (verify(filepath, BSPKG_SHA256))
                *(int*)arg = 1;
            else
                *(int*)arg = 0;
        }, [this](void *arg) {
            if (*(int*)arg)
                startUnpackDemo(BSPKG_FILE);
        }, &n);
    }
}
