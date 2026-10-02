// Endpoint I/O and OSD rendering are isolated from the production manager.
// These tests never register keys, change system volume, or open the live IPC file.
#include "Editor/helpers/VolumeTakeoverManager.h"
#include "Editor/widgets/VolumeOsdWidget.h"

EndpointVolumeState testEndpoint;
bool testEndpointWriteFails = false;
VolumeController::VolumeController(const std::wstring& id) : _requestedEndpointId(id) {}
VolumeController::~VolumeController() = default;
HRESULT VolumeController::getVolumeState(EndpointVolumeState& value) { value = testEndpoint; return S_OK; }
HRESULT VolumeController::getRealEndpointVolumeState(EndpointVolumeState& value) { value = testEndpoint; return S_OK; }
HRESULT VolumeController::getVolume(double& value) { value = testEndpoint.levelDb; return S_OK; }
HRESULT VolumeController::setVolumeScalar(double value)
{
	if (testEndpointWriteFails) return E_FAIL;
	testEndpoint.scalar = value;
	return S_OK;
}
HRESULT VolumeController::setMute(bool value)
{
	if (testEndpointWriteFails) return E_FAIL;
	testEndpoint.muted = value;
	return S_OK;
}
bool VolumeController::hasVolumeChanged() { return false; }

// Follow Off is used throughout; gain calculation must not enter these doubles.
double LoudnessCorrectionFilter::calculateVolumeFollowGain(FilterParameters::VolumeFollowMode, double, double, bool)
{
	qFatal("Unexpected DSP call in isolated volume control test");
	return 0.0;
}
double LoudnessCorrectionFilter::calculateListeningVolumeDb(FilterParameters::VolumeFollowMode, double, double)
{
	qFatal("Unexpected DSP call in isolated volume control test");
	return 0.0;
}
VolumeOsdWidget::VolumeOsdWidget(QWidget* parent) : QWidget(parent) {}
void VolumeOsdWidget::showVolume(double, double, bool, double) {}
void VolumeOsdWidget::paintEvent(QPaintEvent*) {}
void VolumeOsdWidget::mousePressEvent(QMouseEvent*) {}
void VolumeOsdWidget::mouseMoveEvent(QMouseEvent*) {}
void VolumeOsdWidget::mouseReleaseEvent(QMouseEvent*) {}
void VolumeOsdWidget::onScalarAnimationChanged(const QVariant&) {}
void VolumeOsdWidget::onFadeAnimationChanged(const QVariant&) {}
void VolumeOsdWidget::onFadeAnimationFinished() {}
void VolumeOsdWidget::startFadeOut() {}
