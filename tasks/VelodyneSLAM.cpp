/* Generated from orogen/lib/orogen/templates/tasks/Task.cpp */

#include "VelodyneSLAM.hpp"

#include <g2o/core/factory.h>
#include <g2o/core/optimization_algorithm_factory.h>
#include <graph_slam/vertex_se3_gicp.hpp>
#include <graph_slam/edge_se3_gicp.hpp>
#include <velodyne_lidar/pointcloudConvertHelper.hpp>
#include <envire/maps/Pointcloud.hpp>
#include <envire/maps/MLSGrid.hpp>


using namespace graph_slam;

VelodyneSLAM::VelodyneSLAM(std::string const& name)
    : VelodyneSLAMBase(name)
{
}

VelodyneSLAM::VelodyneSLAM(std::string const& name, RTT::ExecutionEngine* engine)
    : VelodyneSLAMBase(name, engine)
{
}

VelodyneSLAM::~VelodyneSLAM()
{
}

void VelodyneSLAM::lidar_samplesTransformerCallback(const base::Time &ts, const ::velodyne_lidar::MultilevelLaserScan &lidar_sample)
{
    // get transformation
    Eigen::Affine3d laser2body;
    if (!_laser2body.get(ts, laser2body))
    {
        std::cerr << "skip, have no laser2body transformation sample!" << std::endl;
        return;
    }
    base::samples::RigidBodyState body2odometry;
    if (!_body2odometry.get(ts, body2odometry, true))
    {
        std::cerr << "skip, have no body2odometry transformation sample!" << std::endl;
        return;
    }

    // TODO the covariance is not handled by the transformer. this should by a temporary solution
    current_position_cov += odometry_position_cov;
    current_orientation_cov += odometry_orientation_cov;
    body2odometry.cov_position = current_position_cov;
    body2odometry.cov_orientation = current_orientation_cov;
    
    if(body2odometry.hasValidPosition() && body2odometry.hasValidOrientation())
    {
        Eigen::Affine3d odometry_delta = last_odometry_sample.getTransform().inverse() * body2odometry.getTransform();
        if(odometry_delta.translation().norm() > _vertex_distance.get() || optimizer.vertices().size() == 0)
        {
            // add point cloud to envire
            envire::Pointcloud* envire_pointcloud = new envire::Pointcloud();
            envire::FrameNode* frame = new envire::FrameNode();
            env->addChild(env->getRootNode(), frame);
            env->setFrameNode(envire_pointcloud, frame);
            if(use_mls)
                env->addInput(projection.get(), envire_pointcloud);
            
            try
            {
                // add new vertex to graph
                velodyne_lidar::ConvertHelper::convertScanToPointCloud(lidar_sample, envire_pointcloud->vertices, laser2body);
                if(!optimizer.addVertex(body2odometry, envire_pointcloud))
                    throw std::runtime_error("failed to add a new vertex");
                
                // run optimization
                if(optimizer.vertices().size() % 5 == 0)
                {
                    if(optimizer.optimize(5) < 1)
                        throw std::runtime_error("optimization failed");
                    
                    // update envire
                    if(!optimizer.updateEnvireTransformations())
                        throw std::runtime_error("can't update envire transformations for one or more vertecies");
                    if(use_mls)
                        projection->updateAll();
                    
                    // find new edges
                    optimizer.findNewEdgesForLastN(5);
                }
                else
                {
                    // update envire
                    if(!optimizer.updateEnvireTransformations())
                        throw std::runtime_error("can't update envire transformations for one or more vertecies");
                }
            }
            catch(std::runtime_error e)
            {
                std::cerr << "Exception while handling lidar sample: " << e.what() << std::endl;
            }
            
            last_odometry_sample = body2odometry;
        }
    }
}

/// The following lines are template definitions for the various state machine
// hooks defined by Orocos::RTT. See VelodyneSLAM.hpp for more detailed
// documentation about them.

bool VelodyneSLAM::configureHook()
{
    if (! VelodyneSLAMBase::configureHook())
        return false;
    
    last_envire_update.microseconds = 0;
    // set map offset to zero
    last_odometry_sample.initUnknown();
    current_position_cov = Eigen::Matrix3d::Zero();
    current_orientation_cov = Eigen::Matrix3d::Zero();
    odometry_position_cov = 0.0003 * Eigen::Matrix3d::Identity();
    odometry_orientation_cov = 0.00000001 * Eigen::Matrix3d::Identity();
    env.reset(new envire::Environment());
    use_mls = _use_mls;
    
    g2o::OptimizableGraph::initMultiThreading();
    
    // set gicp config
    graph_slam::GICPConfiguration gicp_config;
    optimizer.updateGICPConfiguration(gicp_config);
    
    // enable debug output
    optimizer.setVerbose(true);
    
    if(use_mls)
    {
        // setup envire mls grid
        double grid_size_x = 50;
        double grid_size_y = 50;
        double cell_resolution_x = 0.075;
        double cell_resolution_y = 0.075;
        double grid_count_x = grid_size_x / cell_resolution_x;
        double grid_count_y = grid_size_y / cell_resolution_y;
        envire::MultiLevelSurfaceGrid* mls = new envire::MultiLevelSurfaceGrid(grid_count_y, grid_count_x, cell_resolution_x, cell_resolution_y, -0.5 * grid_size_x, -0.5 * grid_size_y);
        projection.reset(new envire::MLSProjection());
        
        env->attachItem(mls);
        envire::FrameNode *fn = new envire::FrameNode();
        env->getRootNode()->addChild(fn);
        mls->setFrameNode(fn);
        env->addOutput(projection.get(), mls);
    }
    
    return true;
}
bool VelodyneSLAM::startHook()
{
    if (! VelodyneSLAMBase::startHook())
        return false;
    
    
    
    return true;
}
void VelodyneSLAM::updateHook()
{
    if( orocos_emitter.use_count() == 0 && _envire_map.connected() )
    {
        // register the binary event dispatcher, 
        // which will write envire data to a port
        orocos_emitter.reset(new envire::OrocosEmitter( _envire_map ));
        orocos_emitter->useContextUpdates( env.get() );
        orocos_emitter->useEventQueue( true );
        orocos_emitter->attach( env.get() );
    }
    
    VelodyneSLAMBase::updateHook();
    
    if( orocos_emitter.use_count() > 0 )
    {
        if( _envire_map.connected() )
        {
            if( (last_envire_update + base::Time::fromSeconds(_envire_period.value())) < base::Time::now() ) 
            {
                orocos_emitter->flush();
                last_envire_update = base::Time::now();
            }
        }
        else
        {
            orocos_emitter.reset();
        }
    }
}
void VelodyneSLAM::errorHook()
{
    VelodyneSLAMBase::errorHook();
}
void VelodyneSLAM::stopHook()
{
    VelodyneSLAMBase::stopHook();
    
    if(_environment_debug_path.get() != "")
        env->serialize(_environment_debug_path.get());
    
}
void VelodyneSLAM::cleanupHook()
{
    VelodyneSLAMBase::cleanupHook();
    
    orocos_emitter.reset();
    
    env.reset();
    
    // freeing the graph memory
    optimizer.clear();

    // destroy all the singletons
    g2o::Factory::destroy();
    g2o::OptimizationAlgorithmFactory::destroy();
    g2o::HyperGraphActionLibrary::destroy();
}