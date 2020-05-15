#include "Pipeline.h"
#include "HSIPixel.h"
#include "IRenderable.h"
#include "IRenderTarget.h"
#include "IGroupContainer.h"
#include "Framebuffer.h"

#include <functional>
#include <iomanip>
#include <sstream>

#include <ctpl_stl.h>

#include "../Logging.h"
#include "../ConfigManager.h"

#include "FillRenderable.h"
#include "GroupTarget.h"

using thread_pool = ctpl::thread_pool;
using namespace Lichtenstein::Server::Render;

// global rendering pipeline :)
std::shared_ptr<Pipeline> Pipeline::sharedInstance;

/**
 * Initializes the pipeline.
 */
void Pipeline::start() {
    XASSERT(!sharedInstance, "Pipeline is already initialized");
    sharedInstance = std::make_shared<Pipeline>();
}

/**
 * Tears down the pipeline at the earliest opportunity.
 */
void Pipeline::stop() {
    sharedInstance->terminate();
    sharedInstance = nullptr;
}



/**
 * Initializes the rendering pipeline.
 */
Pipeline::Pipeline() {
    // allocate the framebuffer
    this->fb = std::make_shared<Framebuffer>();

    // start up our worker thread
    this->shouldTerminate = false;
    this->worker = std::make_unique<std::thread>(&Pipeline::workerEntry, this);
}
/**
 * Cleans up all resources used by the pipeline.
 */
Pipeline::~Pipeline() {
    /* Ensure the worker has terminated when the destructor is called. This in
     * turn means that all render threads have also terminated. */
    if(!this->shouldTerminate) {
        Logging::error("You should call Render::Pipeline::terminate() before deleting");
        this->terminate(); 
    }
    this->worker->join();
}

/**
 * Requests termination of the renderer.
 */
void Pipeline::terminate() {
    // catch repeated calls
    if(this->shouldTerminate) {
        Logging::error("Ignoring repeated call of Render::Pipeline::terminate()!");
        return;
    }

    // set the termination flag
    Logging::debug("Requesting render pipeline termination");
    this->shouldTerminate = true;
}



/**
 * Entry point for the main rendering thread. This thread is responsible for
 * handling the timing of the renders, distributing work to the work queue, and
 * the overall logistics of getting the final values into the framebuffer so
 * that output plugins can be notified.
 *
 * Note that we let the entire run through the loop finish before we check the
 * termination flag, so worst case terminating the renderer may take 1/fps sec.
 */
void Pipeline::workerEntry() {
    using namespace std::chrono;

    // setup for rendering
    this->readConfig();

    this->pool = std::make_unique<thread_pool>(this->numRenderThreads);

    // initialize some counters
    this->actualFps = -1;
    this->actualFramesCounter = 0;
    this->fpsStart = high_resolution_clock::now();

    this->sleepInaccuracy = this->sleepInaccuracySamples = 0;

    // main loop
    while(!this->shouldTerminate) {
        RenderPlan currentPlan;

        // timestamp the start of frame
        auto start = high_resolution_clock::now();

        // get the render plan for this frame
        this->planLock.lock();
        currentPlan = this->plan;
        this->planLock.unlock();

        if(!currentPlan.empty()) {
            auto token = this->fb->startFrame();
            
            // set up for the rendering
            for(auto const &[target, renderable] : currentPlan) {
                renderable->lock();
                renderable->prepare();
                renderable->unlock();
            }

            // dispatch render jobs to the work pool
            std::vector<std::shared_future<void>> jobs;

            for(auto const &[target, renderable] : currentPlan) {
                auto f = this->submitRenderJob(renderable, target);
                jobs.push_back(f);
            }

            // wait for all render jobs to complete and finish
            for(auto const &f : jobs) {
                f.wait();
            }

            for(auto const &[target, renderable] : currentPlan) {
                renderable->lock();
                renderable->finish();
                renderable->unlock();
            }

            this->fb->endFrame(token);
        }

        // sleep to maintain framerate
        this->totalFrames++;
        this->sleep(start);
    }

    // clean up
    Logging::debug("Render pipeline is shutting down");

    this->pool->stop(false);
    this->pool = nullptr;
}

/**
 * Reads all configuration into our instance variables to cache it.
 */
void Pipeline::readConfig() {
    this->targetFps = ConfigManager::getDouble("render.pipeline.fps", 42);
    this->numRenderThreads = ConfigManager::getUnsigned("render.pipeline.threads", 2);

    Logging::debug("Pipeline fps = {:.1f}; using {} render threads", 
            this->targetFps, this->numRenderThreads);
}



/**
 * Submits a single renderable/target pair to the render queue. Returned is a
 * future that can be used to wait for the job to complete.
 */
std::shared_future<void> Pipeline::submitRenderJob(RenderablePtr render,
        TargetPtr target) {
    using namespace std::placeholders;
    XASSERT(render, "Renderable is required");
    XASSERT(target, "Target is required");

    auto fxn = std::bind(&Pipeline::renderOne, this, render, target);
    auto f = this->pool->push(fxn);
    return f.share();
}

/**
 * Executes the renderable's render function, then copies its data into the
 * output framebuffer.
 */
void Pipeline::renderOne(RenderablePtr renderable, TargetPtr target) {
    // acquire locks
    renderable->lock();

    // render and copy data out
    renderable->render();
    target->inscreteFrame(this->fb, renderable);

    // release locks
    renderable->unlock();
}




/**
 * Sleeps enough time to maintain our framerate. This will compensate for drift
 * or other inaccuracies.
 */
void Pipeline::sleep(Timestamp startOfFrame) {
    using namespace std::chrono;

    struct timespec sleep;
    sleep.tv_sec = 0;

    double fpsTimeNs = ((1000 * 1000 * 1000) / this->targetFps);
 
    // sleep for the amount of time required
    auto end = high_resolution_clock::now();
    duration<double, std::nano> difference = (end - startOfFrame);
    long differenceNanos = difference.count();

    sleep.tv_nsec = (fpsTimeNs - differenceNanos);
    sleep.tv_nsec -= this->sleepInaccuracy;
    nanosleep(&sleep, nullptr);

    auto wokenUp = high_resolution_clock::now();

    // calculate fps and irl sleep time
    this->computeActualFps();

    auto sleepTime = high_resolution_clock::now() - end;
    duration<double, std::micro> micros = sleepTime;

    double sleepTimeUs = micros.count();
    double sleepTimeNs = duration<double, std::nano>(sleepTime).count();

    this->compensateSleep(sleep.tv_nsec, sleepTimeNs);
}

/*
 * Naiively calculate a compensation to apply to the nanosleep() call to get us
 * as close as possible to the desired sleep time. We calculate a moving
 * average on the difference between the actual and requested sleep.
 *
 * This algorithm does not handle sudden lag spikes very well.
 */
void Pipeline::compensateSleep(long requested, long actual) {
	double difference = (actual - requested);

	double n = this->sleepInaccuracySamples;
	double oldAvg = this->sleepInaccuracy;

	double newAvg = ((oldAvg * n) + difference) / (n + 1);

	this->sleepInaccuracy = newAvg;
	this->sleepInaccuracySamples++;
}

/**
 * Calculates the actual fps. This counts the number of frames that get
 * processed in a one-second span, and extrapolates fps from this.
 */
void Pipeline::computeActualFps() {
    using namespace std::chrono;

    // increment frames counter and get time difference since measurement start
    this->actualFramesCounter++;

    auto current = high_resolution_clock::now();
    duration<double, std::milli> fpsDifference = (current - this->fpsStart);

    if(fpsDifference.count() >= 1000) {
        // calculate the actual fps and reset counter
        this->actualFps = double(this->actualFramesCounter) /
            (fpsDifference.count() / 1000.f);

        this->actualFramesCounter = 0;
        this->fpsStart = high_resolution_clock::now();
    }
}



/**
 * Adds a mapping of renderable -> target to be dealt with the next time we
 * render a frame.
 *
 * This will ensure that no output group is specified twice. If there is a
 * mapping with the same target, it will be replaced.
 */
void Pipeline::add(RenderablePtr renderable, TargetPtr target) {
    using std::dynamic_pointer_cast;

    if(!renderable) {
        throw std::invalid_argument("Renderable is required");
    } else if(!target) {
        throw std::invalid_argument("Target is required");
    }

    std::lock_guard<std::mutex> lg(this->planLock);

    auto inContainer = dynamic_pointer_cast<IGroupContainer>(target);

    // is the input mapping a container?
    if(inContainer) {
        // iterate over all targets and see if they intersect with this one
        for(auto it = this->plan.cbegin(); it != this->plan.cend(); ) {
            auto t = it->first;
            auto const r = it->second;

            // is this entry a group container?
            auto c = dynamic_pointer_cast<IGroupContainer>(t);
            if(!c) {
                goto next;
            }

            // check whether there are any conflicts
            if(c->contains(*inContainer)) {
                Logging::debug("Conflict between input {} and entry {}",
                        *inContainer, *c);

                // are the groups identical?
                if(*c == *inContainer) {
                    // if so, replace this entry
                    Logging::trace("Identical groups in existing container; removing existing");
                    it = this->plan.erase(it);
                    goto beach;
                }
                // they aren't, but the container is mutable
                else if(c->isMutable()) {
                    // update the target by removing the intersection
                    std::vector<int> intersection;
                    c->getUnion(*inContainer, intersection);

                    Logging::trace("Removing {} groups from conflicting entry",
                            intersection.size());

                    t->lock();
                    for(const int id : intersection) {
                        c->removeGroup(id);
                    }
                    t->unlock();

                    // if this emptied the conflicting group, remove it
                    if(t->numPixels() == 0) {
                        Logging::trace("Removing empty conflicting target and inserting");
                        it = this->plan.erase(it);
                        continue;
                    }

                    // take a lock on the renderable and resize
                    r->lock();

                    auto requiredSize = t->numPixels();
                    Logging::trace("Resizing renderable {} to {} pixels",
                            (void*) r.get(), requiredSize);
                    r->resize(requiredSize);

                    r->unlock();
                }
                // if the conflicting container is immutable but only one remove it
                else if(c->numGroups() == 1) {
                    Logging::trace("Removing single group conflicting entry");

                    it = this->plan.erase(it);
                    continue;
                }
                // container is immutable so we can't handle this
                else {
                    Logging::trace("Immutable container, cannot satisfy mapping");
                    throw std::runtime_error("Unable to add mapping");
                }
            }

            // there are no conflicts with this container, check the next
next: ;
            ++it;
        }

beach: ;
        // if we get here, go ahead and insert it. conflicts have been resolved
        this->plan[target] = renderable;
    }
    // it is not; we should just insert it
    else {
        Logging::warn("Inserting non-container render target {}",
                (void*)target.get());

        this->plan[target] = renderable;
    }
}
/**
 * Removes the mapping to the given target.
 */
void Pipeline::remove(TargetPtr target) {
    if(!target) {
        throw std::invalid_argument("Target is required");
    }

    std::lock_guard<std::mutex> lg(this->planLock);

    if(this->plan.find(target) == this->plan.end()) {
        throw std::invalid_argument("No such target in render pipeline");
    }
    
    this->plan.erase(target);
}

/**
 * Adds a single group with the given renderable to the pipeline.
 */
Pipeline::TargetPtr Pipeline::add(RenderablePtr renderable, const Group &g) {
    auto t = std::make_shared<GroupTarget>(g);
    this->add(renderable, t);
    return t;
}

/**
 * Creates a multigroup from the specified list of groups ands adds it to the
 * render pipeline.
 */
Pipeline::TargetPtr Pipeline::add(RenderablePtr renderable, 
        const std::vector<Group> &g) {
    auto t = std::make_shared<MultiGroupTarget>(g);
    this->add(renderable, t);
    return t;
}




/**
 * Dumps the current output mapping to the log output.
 */
void Pipeline::dump() {
    std::lock_guard<std::mutex> lg(this->planLock);
    std::stringstream out;
        
    // loop over the entire plan
    for(auto it = this->plan.begin(); it != this->plan.end(); it++) {
        if(it != this->plan.begin()) out << std::endl;
            
        auto target = it->first;
        auto renderable = it->second;

        auto container = std::dynamic_pointer_cast<IGroupContainer>(target);

        // print container value
        out << std::setw(20) << std::setfill(' ');
        if(container) {
            out << *container;
        } else {
            out << std::hex << (uintptr_t) target.get();
        }
        out << std::setw(0);

        // print the renderable
        out << " 0x" << std::hex << (uintptr_t) renderable.get();
    }

    // print that shit
    Logging::debug("Pipeline state\n{}", out.str());
}

