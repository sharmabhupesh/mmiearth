/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "ObjectIDPicker"
#include "Shaders"
#include "ExampleResources"
#include "GLUtils"
#include "Utils"
#include "CameraUtils"
#include "VirtualProgram"
#include "Registry"

#include <osg/BlendFunc>

using namespace osgEarth;
using namespace osgEarth::Util;

#define LC "[ObjectIDPicker] "

namespace
{
    // Iterates through the pixels in a grid, starting at u,v [0..1] and spiraling out.
    // It will stop when it reaches the "max ring", which is basically a distance from
    // the starting point.
    // Inspiration: http://stackoverflow.com/a/14010215/4218920
    struct SpiralIterator
    {
        unsigned _ring;
        unsigned _maxRing;
        unsigned _leg;
        int      _x, _y;
        int      _w, _h;
        int      _offsetX, _offsetY;
        unsigned _count;

        SpiralIterator(int w, int h, int maxDist, float u, float v) : 
            _w(w), _h(h), _maxRing(maxDist), _count(0), _ring(1), _leg(0), _x(0), _y(0)
        {
            _offsetX = (int)(u * (float)w);
            _offsetY = (int)(v * (float)h);
        }

        bool next()
        {
            // first time, just use the start point
            if (_count == 0)
            {
                if (_offsetX < 0 || _offsetX >= _w || _offsetY < 0 || _offsetY >= _h)
                    return false;
                else
                {
                    _count++;
                    return true;
                }
            }

            // spiral until we get to the next valid in-bounds pixel:
            do {
                switch(_leg) {
                case 0: ++_x; if (  _x == (int)_ring ) ++_leg; break;
                case 1: ++_y; if (  _y == (int)_ring ) ++_leg; break;
                case 2: --_x; if ( -_x == (int)_ring ) ++_leg; break;
                case 3: --_y; if ( -_y == (int)_ring ) { _leg = 0; ++_ring; } break;
                }
            }
            while(_ring <= _maxRing && (_x+_offsetX < 0 || _x+_offsetX >= _w || _y+_offsetY < 0 || _y+_offsetY >= _h));

            return _ring <= _maxRing;
        }

        int s() const { return _x+_offsetX; }

        int t() const { return _y+_offsetY; }
    };
}


ObjectIDPicker::ObjectIDPicker()
{
    setCullingActive(false);
}

ObjectIDPicker::~ObjectIDPicker()
{
    setView(nullptr);
}

void
ObjectIDPicker::setView(osgViewer::View* view)
{
    OE_SOFT_ASSERT_AND_RETURN(_rttSize >= 1, void());

    if (view != _view.get())
    {
        _view = view;

        if (view)
        {
            osg::observer_ptr<ObjectIDPicker> weak(this);

            EventRouter::get(view).onMove(
                [weak](osg::View* view, float x, float y)
                {
                    osg::ref_ptr<ObjectIDPicker> safe;
                    if (weak.lock(safe))
                    {
                        if (safe->getNodeMask() > 0 && view == safe->_view.get())
                        {
                            safe->pick(view, x, y, safe->ACTION_HOVER);
                        }
                    }
                }
            );

            EventRouter::get(view).onClick(
                [weak](osg::View* view, float x, float y)
                {
                    osg::ref_ptr<ObjectIDPicker> safe;
                    if (weak.lock(safe))
                    {
                        if (safe->getNodeMask() > 0 && view == safe->_view.get())
                        {
                            safe->pick(view, x, y, safe->ACTION_CLICK);
                        }
                    }
                },
                false // don't eat the event
            );

            // if we have no graph, install one by default
            if (_graph.valid() == false)
            {
                _graph = view->getSceneData();
            }

            setupRTT(view);
        }
    }
}

void
ObjectIDPicker::setGraph(osg::Node* value)
{
    _graph = value;

    // remove old graph
    if (_rtt.valid())
        _rtt->removeChildren(0, _rtt->getNumChildren());

    // install new graph
    if (value)
        _rtt->addChild(value);
}

void
ObjectIDPicker::pick(osg::View* view, float x, float y, ActionType action)
{
    ImageUtils::PixelReader read(_pickImage.get());

    OE_SOFT_ASSERT_AND_RETURN(view->getNumSlaves() == 0, void(), "ObjectIDPicker does not support an OSG slave-camera configuration");

    const osg::Viewport* vp = view->getCamera()->getViewport();
    float u = (x - vp->x()) / vp->width();
    float v = (y - vp->y()) / vp->height();

    bool hit = false;
    osg::Vec4f value;
    SpiralIterator iter(
        _pickImage->s(), _pickImage->t(),
        osg::maximum(_buffer, 1),
        u, v);

    while (iter.next() && (hit == false))
    {
        read(value, iter.s(), iter.t());

        ObjectID id = (ObjectID)(
            ((unsigned)(value.r()*255.0) << 24) +
            ((unsigned)(value.g()*255.0) << 16) +
            ((unsigned)(value.b()*255.0) << 8) +
            ((unsigned)(value.a()*255.0)));

        if (id > 0)
        {
            onPick.fire(id, action);

            // backwards compat
            OE_START_DEPRECATED_FUNCTION_CALLS;
            if (action == ACTION_HOVER)
                onHover.fire(id);
            else
                onClick.fire(id);
            OE_END_DEPRECATED_FUNCTION_CALLS;

            hit = true;
        }
    }

    if (!hit)
    {
        // missed, so fire the functions with id=0 (empty)
        onPick.fire(OSGEARTH_OBJECTID_EMPTY, action);

        // backwards compatibility
        OE_START_DEPRECATED_FUNCTION_CALLS;
        if (action == ACTION_HOVER)
            onHover.fire(OSGEARTH_OBJECTID_EMPTY);
        else
            onClick.fire(OSGEARTH_OBJECTID_EMPTY);
        OE_END_DEPRECATED_FUNCTION_CALLS;
    }
}

void
ObjectIDPicker::setupRTT(osgViewer::View* view)
{
    _pickImage = new osg::Image();
    _pickImage->allocateImage(_rttSize, _rttSize, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    memset(_pickImage->data(), 0, _pickImage->getTotalSizeInBytes());

    // Make an RTT camera and bind it to our image.
    _rtt = new osg::Camera();
    CameraUtils::setIsPickCamera(_rtt.get());
    _rtt->setView(view); // so we have access to the 'real' viewport dimensions
    _rtt->setName("osgEarth.ObjectIDPicker");
    _rtt->addChild(_graph.get());
    _rtt->setClearColor(osg::Vec4(0, 0, 0, 0));
    _rtt->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    _rtt->setViewport(0, 0, _rttSize, _rttSize);
    _rtt->setRenderOrder(osg::Camera::POST_RENDER);
    _rtt->setReferenceFrame(osg::Transform::ABSOLUTE_RF_INHERIT_VIEWPOINT);
    _rtt->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
    _rtt->attach(osg::Camera::COLOR_BUFFER0, _pickImage.get());
    _rtt->setSmallFeatureCullingPixelSize(-1.0f);

    osg::StateSet* rttSS = _rtt->getOrCreateStateSet();

    // disable all the things that break ObjectID picking:
    osg::StateAttribute::GLModeValue disable =
        osg::StateAttribute::OFF | 
        osg::StateAttribute::OVERRIDE | 
        osg::StateAttribute::PROTECTED;

    GLUtils::setLighting(rttSS, disable);
    rttSS->setMode(GL_CULL_FACE, disable);
    rttSS->setMode(GL_ALPHA_TEST, disable);

#if !(defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE) )
    rttSS->setMode(GL_POINT_SMOOTH, disable);
    rttSS->setMode(GL_LINE_SMOOTH, disable);
#endif

    // Disabling GL_BLEND is not enough, because osg::Text re-enables it
    // without regard for the OVERRIDE.
    rttSS->setAttributeAndModes(
        new osg::BlendFunc(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO),
        osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);

    // install the picking shaders:
    VirtualProgram* vp = new VirtualProgram();
    vp->setName(typeid(*this).name());
    Shaders shaders;
    shaders.load(vp, shaders.RTTPicker);
    // Install shaders and bindings from the ObjectIndex:
    Registry::objectIndex()->loadShaders(vp);
    rttSS->setAttribute(vp);

    // designate this as a pick camera
    rttSS->setDefine("OE_LIGHTING", osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

    // default value for the objectid override uniform:
    rttSS->addUniform(new osg::Uniform(Registry::objectIndex()->getObjectIDUniformName().c_str(), 0u));
}

void
ObjectIDPicker::traverse(osg::NodeVisitor& nv)
{
    if (nv.getVisitorType() == nv.CULL_VISITOR)
    {
        if (_rtt.valid() && _view.valid())
        {
            osg::ref_ptr<ObjectIDPicker> temp;
            if (!ObjectStorage::get(&nv, temp))
            {
                ObjectStorage::set(&nv, this);

                _rtt->setProjectionResizePolicy(_view->getCamera()->getProjectionResizePolicy());
                _rtt->setProjectionMatrix(_view->getCamera()->getProjectionMatrix());
                _rtt->setViewMatrix(_view->getCamera()->getViewMatrix());
                _rtt->inheritCullSettings(*(_view->getCamera()), _view->getCamera()->getInheritanceMask());

                _rtt->accept(nv);

                ObjectStorage::remove(&nv, this);
            }
        }
    }
}

osg::Texture2D*
ObjectIDPicker::getOrCreateTexture()
{
    if (!_debugTex.valid() && _pickImage.valid())
    {
        _debugTex = new osg::Texture2D(_pickImage.get());
        _debugTex->setTextureSize(_pickImage->s(), _pickImage->t());
        _debugTex->setUnRefImageDataAfterApply(false);
        _debugTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST); // no filtering
        _debugTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST); // no filtering
        _debugTex->setMaxAnisotropy(1.0f); // no filtering
    }
    return _debugTex.get();
}