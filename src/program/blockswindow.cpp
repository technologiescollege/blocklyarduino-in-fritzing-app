#include "blockswindow.h"

#include <QWebEngineView>
#include <QWebEngineSettings>
#include <QWebEngineProfile>
#include <QWebEngineDownloadRequest>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QSettings>
#include <QCloseEvent>
#include <QVBoxLayout>
#include <QWidget>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QLocale>
#include <QFileDialog>
#include "programwindow.h"
#include "programtab.h"
#include "platformarduino.h"
#include <QApplication>
#include <QClipboard>
#include "../debugdialog.h"

BlocksPage::BlocksPage(QObject *parent)
	: QWebEnginePage(parent)
	, m_checkTimer(nullptr)
{
	m_checkTimer = new QTimer(this);
	connect(m_checkTimer, SIGNAL(timeout()), this, SLOT(checkForCode()));
	m_checkTimer->start(100); // Vérifier toutes les 100ms
}

bool BlocksPage::acceptNavigationRequest(const QUrl &url, QWebEnginePage::NavigationType type, bool isMainFrame)
{
	// Autoriser toutes les navigations normales, y compris les téléchargements
	return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
}

QWebEnginePage *BlocksPage::createWindow(QWebEnginePage::WebWindowType /*type*/)
{
	// Permettre l'ouverture de nouvelles fenêtres/onglets (nécessaire pour les téléchargements)
	// Retourner nullptr pour que Qt gère les téléchargements via downloadRequested
	return nullptr;
}

void BlocksPage::checkForCode()
{
	// Vérifier si du code a été stocké dans la variable JavaScript globale
	runJavaScript("window.blocklyCodeToCopy || ''", [this](const QVariant &result) {
		QString code = result.toString();
		if (!code.isEmpty() && code != m_lastCode) {
			m_lastCode = code;
			DebugDialog::debug(QString("BlocksPage::checkForCode: Found code, length=%1, first 100 chars: %2").arg(code.length()).arg(code.left(100)));
			// Effacer la variable JavaScript pour éviter de traiter le même code plusieurs fois
			runJavaScript("window.blocklyCodeToCopy = '';");
			Q_EMIT codeReceived(code);
		}
	});
}

BlocksWindow::BlocksWindow(QWidget *parent)
	: QDialog(parent)
	, m_javaScriptInjected(false)
{
	QFile styleSheet(":/resources/styles/programwindow.qss");

	this->setObjectName("blocksWindow");
	this->setWindowTitle(tr("Blocks Editor"));
	this->setModal(false); // Rendre la fenêtre non modale
	
	// Ajouter les boutons de minimisation et maximisation
	Qt::WindowFlags flags = windowFlags();
	flags |= Qt::WindowMinimizeButtonHint;
	flags |= Qt::WindowMaximizeButtonHint;
	setWindowFlags(flags);

	if (!styleSheet.open(QIODevice::ReadOnly)) {
		qWarning("Unable to open :/resources/styles/programwindow.qss");
	} else {
		QString ss = styleSheet.readAll();
#ifdef Q_OS_MACOS
		int paneLoc = 4;
		int tabBarLoc = 0;
#else
		int paneLoc = -1;
		int tabBarLoc = 5;
#endif
		ss = ss.arg(paneLoc).arg(tabBarLoc);
		this->setStyleSheet(ss);
	}

	// Créer un layout pour le dialog
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	// Créer une page personnalisée qui intercepte les navigations
	BlocksPage *customPage = new BlocksPage(this);
	connect(customPage, SIGNAL(codeReceived(QString)), this, SLOT(onCodeReceived(QString)));

	// Créer le QWebEngineView avec la page personnalisée
	m_webView = new QWebEngineView(this);
	m_webView->setPage(customPage);

	// Configurer les paramètres WebEngine
	QWebEngineSettings *settings = m_webView->settings();
	settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
	settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
	settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
	settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, true);

	// Gérer les téléchargements
	QWebEngineProfile *profile = m_webView->page()->profile();
	connect(profile, &QWebEngineProfile::downloadRequested, [this](QWebEngineDownloadRequest *download) {
		QString suggestedFileName = download->downloadFileName();
		if (suggestedFileName.isEmpty()) {
			suggestedFileName = "download";
		}
		
		// Ouvrir une boîte de dialogue pour choisir où enregistrer le fichier
		QString savePath = QFileDialog::getSaveFileName(
			this,
			tr("Enregistrer le fichier"),
			QDir::home().absoluteFilePath(suggestedFileName),
			tr("Tous les fichiers (*.*)")
		);
		
		if (!savePath.isEmpty()) {
			download->setDownloadFileName(savePath);
			download->accept();
			DebugDialog::debug(QString("BlocksWindow: Download started: %1").arg(savePath));
		} else {
			// L'utilisateur a annulé
			download->cancel();
		}
	});

	// Connecter le signal de chargement terminé
	connect(m_webView->page(), SIGNAL(loadFinished(bool)), this, SLOT(onLoadFinished(bool)));

	layout->addWidget(m_webView);

	// Charger le fichier index.html avec le paramètre de langue
	QString appDirPath = QCoreApplication::applicationDirPath();
	QDir appDir(appDirPath);
	QString blocklyPath = appDir.absoluteFilePath("blockly");
	QString indexHtmlPath = QDir(blocklyPath).absoluteFilePath("index_fritzing.html");

	QFileInfo fileInfo(indexHtmlPath);
	if (fileInfo.exists() && fileInfo.isFile()) {
		QUrl url = QUrl::fromLocalFile(fileInfo.canonicalFilePath());
		
		// Récupérer la langue sélectionnée dans les préférences
		QSettings settings;
		QString language = settings.value("language").toString();
		if (language.isEmpty()) {
			language = QLocale::system().name();
		}
		
		// Extraire le code langue (ex: "fr" depuis "fr_FR")
		QString languageCode = language.toLower();
		int underscorePos = languageCode.indexOf('_');
		if (underscorePos > 0) {
			languageCode = languageCode.left(underscorePos);
		}
		
		// Ajouter le paramètre de langue à l'URL
		QUrlQuery query;
		query.addQueryItem("lang", languageCode);
		
		// Récupérer la carte sélectionnée depuis ProgramTab
		ProgramWindow *programWindow = qobject_cast<ProgramWindow *>(this->parent());
		if (programWindow != nullptr) {
			ProgramTab *currentTab = programWindow->getCurrentTab();
			if (currentTab != nullptr) {
				Platform *platform = currentTab->platform();
				if (platform != nullptr && platform->getName() == "Arduino") {
					QString boardName = currentTab->board();
					if (!boardName.isEmpty()) {
						QMap<QString, QString> boards = platform->getBoards();
						QString boardId = boards.value(boardName);
						if (!boardId.isEmpty()) {
							// Transformer "arduino:avr:uno" en "arduino_uno"
							// Prendre les deux dernières parties séparées par ":"
							QStringList parts = boardId.split(':');
							if (parts.size() >= 2) {
								QString boardParam = parts[parts.size() - 3] + "_" + parts[parts.size() - 1];
								query.addQueryItem("board", boardParam);
								DebugDialog::debug(QString("BlocksWindow: Adding board parameter: %1").arg(boardParam));
							}
						}
					}
				}
			}
		}
		
		url.setQuery(query);
		m_webView->load(url);
	}

	// Restaurer la géométrie de la fenêtre
	QSettings settings_obj;
	if (!settings_obj.value("blockswindow/geometry").isNull()) {
		restoreGeometry(settings_obj.value("blockswindow/geometry").toByteArray());
	} else {
		// Taille par défaut
		resize(1024, 768);
	}
}

BlocksWindow::~BlocksWindow()
{
	// Sauvegarder la géométrie de la fenêtre
	QSettings settings;
	settings.setValue("blockswindow/geometry", saveGeometry());
}

void BlocksWindow::closeEvent(QCloseEvent *event)
{
	// Sauvegarder la géométrie avant de fermer
	QSettings settings;
	settings.setValue("blockswindow/geometry", saveGeometry());
	QDialog::closeEvent(event);
}

void BlocksWindow::onLoadFinished(bool success)
{
	if (success) {
		// Réinitialiser le flag pour permettre la réinjection après un rafraîchissement
		m_javaScriptInjected = false;
		// Attendre un peu pour que le DOM soit complètement chargé
		QTimer::singleShot(500, this, SLOT(injectJavaScript()));
	}
}

void BlocksWindow::injectJavaScript()
{
	if (m_javaScriptInjected || m_webView == nullptr) {
		return;
	}

	// Injecter le JavaScript pour intercepter le clic sur le bouton
	// Utiliser une URL personnalisée qui sera interceptée par acceptNavigationRequest
	QString script = R"(
		(function() {
			// Fonction pour configurer le bouton de copie
			function setupCopyButton() {
				var copyButton = document.getElementById('btn_CopyCode');
				if (copyButton) {
					// Vérifier si le listener a déjà été ajouté en vérifiant un attribut data
					if (copyButton.hasAttribute('data-qt-listener-added')) {
						return true; // Déjà configuré
					}
					
					// Marquer le bouton comme ayant un listener
					copyButton.setAttribute('data-qt-listener-added', 'true');
					
					// Ajouter le listener sans cloner le bouton pour préserver les autres listeners
					copyButton.addEventListener('click', function(e) {
						// Récupérer le contenu de la div pre_previewArduino
						var preElement = document.getElementById('pre_previewArduino');
						if (preElement) {
							var code = preElement.textContent || preElement.innerText || '';
							// Stocker le code dans une variable globale que Qt pourra lire
							window.blocklyCodeToCopy = code;
						}
						// Ne pas empêcher la propagation pour permettre aux autres handlers de fonctionner
					}, true); // Utiliser capture phase pour exécuter notre handler en premier
					return true;
				}
				return false;
			}

			// Essayer immédiatement
			if (!setupCopyButton()) {
				// Si le bouton n'existe pas encore, attendre un peu et réessayer
				setTimeout(function() {
					setupCopyButton();
				}, 100);
			}
		})();
	)";
	
	m_webView->page()->runJavaScript(script);
	
	m_javaScriptInjected = true;
}

void BlocksWindow::onCodeReceived(const QString &code)
{
	DebugDialog::debug(QString("BlocksWindow::onCodeReceived called with code length: %1").arg(code.length()));
	
	// Copier dans le presse-papier
	QClipboard *clipboard = QApplication::clipboard();
	clipboard->setText(code);
	
	// Envoyer le code dans l'onglet code de la fenêtre principale
	ProgramWindow *programWindow = qobject_cast<ProgramWindow *>(parent());
	if (programWindow != nullptr) {
		DebugDialog::debug("BlocksWindow::onCodeReceived - programWindow found, calling insertCodeIntoCurrentTab");
		programWindow->insertCodeIntoCurrentTab(code);
	} else {
		DebugDialog::debug("BlocksWindow::onCodeReceived - programWindow is nullptr!");
	}
}
