/* ----------------------------------------------------------------------------
 * Copyright 2018, Ross Hartley <m.ross.hartley@gmail.com>
 * All Rights Reserved
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

/**
 *  @file   InEKF.cpp
 *  @author Ross Hartley
 *  @brief  Source file for Invariant EKF 
 *  @date   September 25, 2018
 **/

#include "InEKF.h"

namespace inekf {

using namespace std;

void removeRowAndColumn(Eigen::MatrixXd& M, int index);

// Default constructor
InEKF::InEKF() : g_((Eigen::VectorXd(3) << 0,0,-9.81).finished()), magnetic_field_((Eigen::VectorXd(3) << 0,0,0).finished()) {}

// Constructor with noise params
InEKF::InEKF(NoiseParams params) : g_((Eigen::VectorXd(3) << 0,0,-9.81).finished()), magnetic_field_((Eigen::VectorXd(3) << 0,0,0).finished()), noise_params_(params) {}

// Constructor with initial state
InEKF::InEKF(RobotState state) : g_((Eigen::VectorXd(3) << 0,0,-9.81).finished()), magnetic_field_((Eigen::VectorXd(3) << 0,0,0).finished()), state_(state) {}

// Constructor with initial state and noise params
InEKF::InEKF(RobotState state, NoiseParams params) : g_((Eigen::VectorXd(3) << 0,0,-9.81).finished()), magnetic_field_((Eigen::VectorXd(3) << 0,0,0).finished()), state_(state), noise_params_(params) {}

// Clear all data in the filter
void InEKF::clear() {
    state_ = RobotState();
    noise_params_ = NoiseParams();
    prior_landmarks_.clear();
    estimated_landmarks_.clear();
    contacts_.clear();
    estimated_contact_positions_.clear();
}

// Return robot's current state
RobotState InEKF::getState() const { return state_; }

// Sets the robot's current state
void InEKF::setState(RobotState state) { state_ = state; }

// Return noise params
NoiseParams InEKF::getNoiseParams() const { return noise_params_; }

// Sets the filter's noise parameters
void InEKF::setNoiseParams(NoiseParams params) { noise_params_ = params; }

// Return filter's prior (static) landmarks
mapIntVector3d InEKF::getPriorLandmarks() const { return prior_landmarks_; }

// Set the filter's prior (static) landmarks
void InEKF::setPriorLandmarks(const mapIntVector3d& prior_landmarks) { prior_landmarks_ = prior_landmarks; }

// Return filter's estimated landmarks
map<int,int> InEKF::getEstimatedLandmarks() const { return estimated_landmarks_; }

// Return filter's estimated landmarks
map<int,int> InEKF::getEstimatedContactPositions() const { return estimated_contact_positions_; }

// Set the filter's contact state
void InEKF::setContacts(vector<pair<int,bool> > contacts) {
    // Insert new measured contact states
    for (vector<pair<int,bool> >::iterator it=contacts.begin(); it!=contacts.end(); ++it) {
        pair<map<int,bool>::iterator,bool> ret = contacts_.insert(*it);
        // If contact is already in the map, replace with new value
        if (ret.second==false) {
            ret.first->second = it->second;
        }
    }
    return;
}

// Return the filter's contact state
std::map<int,bool> InEKF::getContacts() const { return contacts_; }

// Set the true magnetic field
void InEKF::setMagneticField(Eigen::Vector3d& true_magnetic_field) { magnetic_field_ = true_magnetic_field; }

// Get the true magnetic field
Eigen::Vector3d InEKF::getMagneticField() const { return magnetic_field_; }

// InEKF Propagation - Inertial Data
void InEKF::Propagate(const Eigen::Matrix<double,6,1>& imu, double dt) {

    Eigen::Vector3d w = imu.head(3) - state_.getGyroscopeBias();    // Angular Velocity
    Eigen::Vector3d a = imu.tail(3) - state_.getAccelerometerBias(); // Linear Acceleration
    
    Eigen::MatrixXd X = state_.getX();
    Eigen::MatrixXd P = state_.getP();

    // Extract State
    Eigen::Matrix3d R = state_.getRotation();
    Eigen::Vector3d v = state_.getVelocity();
    Eigen::Vector3d p = state_.getPosition();

    // Strapdown IMU motion model
    Eigen::Vector3d phi = w*dt; 
    Eigen::Matrix3d R_pred = R * Exp_SO3(phi);
    Eigen::Vector3d v_pred = v + (R*a + g_)*dt;
    Eigen::Vector3d p_pred = p + v*dt + 0.5*(R*a + g_)*dt*dt;

    // Set new state (bias has constant dynamics)
    state_.setRotation(R_pred);
    state_.setVelocity(v_pred);
    state_.setPosition(p_pred);

    // ---- Linearized invariant error dynamics -----
    int dimX = state_.dimX();
    int dimP = state_.dimP();
    int dimTheta = state_.dimTheta();
    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(dimP,dimP);
    // Inertial terms
    A.block<3,3>(3,0) = skew(g_); // TODO: Efficiency could be improved by not computing the constant terms every time
    A.block<3,3>(6,3) = Eigen::Matrix3d::Identity();
    // Bias terms
    A.block<3,3>(0,dimP-dimTheta) = -R;
    A.block<3,3>(3,dimP-dimTheta+3) = -R;
    for (int i=3; i<dimX; ++i) {
        A.block<3,3>(3*i-6,dimP-dimTheta) = -skew(X.block<3,1>(0,i))*R;
    } 

    // Noise terms
    Eigen::MatrixXd Qk = Eigen::MatrixXd::Zero(dimP,dimP); // Landmark noise terms will remain zero
    Qk.block<3,3>(0,0) = noise_params_.getGyroscopeCov(); 
    Qk.block<3,3>(3,3) = noise_params_.getAccelerometerCov();
    for(map<int,int>::iterator it=estimated_contact_positions_.begin(); it!=estimated_contact_positions_.end(); ++it) {
        Qk.block<3,3>(3+3*(it->second-3),3+3*(it->second-3)) = noise_params_.getContactCov(); // Contact noise terms
    } // TODO: Use kinematic orientation to map noise from contact frame to body frame
    Qk.block<3,3>(dimP-dimTheta,dimP-dimTheta) = noise_params_.getGyroscopeBiasCov();
    Qk.block<3,3>(dimP-dimTheta+3,dimP-dimTheta+3) = noise_params_.getAccelerometerBiasCov();

    // Discretization
    Eigen::MatrixXd I = Eigen::MatrixXd::Identity(dimP,dimP);
    Eigen::MatrixXd Phi = I + A*dt; // Fast approximation of exp(A*dt). TODO: explore using the full exp() instead
    Eigen::MatrixXd Adj = I;
    Adj.block(0,0,dimP-dimTheta,dimP-dimTheta) = Adjoint_SEK3(X); 
    Eigen::MatrixXd PhiAdj = Phi * Adj;
    Eigen::MatrixXd Qk_hat = PhiAdj * Qk * PhiAdj.transpose() * dt; // Approximated discretized noise matrix

    // Propagate Covariance
    Eigen::MatrixXd P_pred = Phi * P * Phi.transpose() + Qk_hat;

    // Set new covariance
    state_.setP(P_pred);

    return;
}

// Correct State: Right-Invariant Observation
void InEKF::CorrectRightInvariant(const Observation& obs) {
    // Compute Kalman Gain
    Eigen::MatrixXd P = state_.getP();
    Eigen::MatrixXd PHT = P * obs.H.transpose();
    Eigen::MatrixXd S = obs.H * PHT + obs.N;
    Eigen::MatrixXd K = PHT * S.inverse();

    // Copy X along the diagonals if more than one measurement
    Eigen::MatrixXd BigX;
    state_.copyDiagX(obs.Y.rows()/state_.dimX(), BigX);
   
    // Compute correction terms
    Eigen::MatrixXd Z = BigX*obs.Y - obs.b;
    Eigen::VectorXd delta = K*obs.PI*Z;
    Eigen::MatrixXd dX = Exp_SEK3(delta.segment(0,delta.rows()-state_.dimTheta()));
    Eigen::VectorXd dTheta = delta.segment(delta.rows()-state_.dimTheta(), state_.dimTheta());

    // Update state
    Eigen::MatrixXd X_new = dX*state_.getX(); // Right-Invariant Update
    Eigen::VectorXd Theta_new = state_.getTheta() + dTheta;
    state_.setX(X_new); 
    state_.setTheta(Theta_new);

    // Update Covariance
    Eigen::MatrixXd IKH = Eigen::MatrixXd::Identity(state_.dimP(),state_.dimP()) - K*obs.H;
    Eigen::MatrixXd P_new = IKH * P * IKH.transpose() + K*obs.N*K.transpose(); // Joseph update form
    state_.setP(P_new); 
}   


// Correct State: Left-Invariant Observation
void InEKF::CorrectLeftInvariant(const Observation& obs) {

    // Compute Kalman Gain
    Eigen::MatrixXd P = state_.getP();
    // For now, the covariance is always assumed to be right-invariant
    // therefore, we need to map it temporarily to left-invariant for this correction
    int dimP = state_.dimP();
    int dimTheta = state_.dimTheta();
    Eigen::MatrixXd Adj = Eigen::MatrixXd::Identity(dimP,dimP);
    Adj.block(0,0,dimP-dimTheta,dimP-dimTheta) = Adjoint_SEK3(state_.getX().inverse()); // TODO: move to analytical inverse
    P = (Adj * P * Adj.transpose()).eval(); 

    Eigen::MatrixXd PHT = P * obs.H.transpose();
    Eigen::MatrixXd S = obs.H * PHT + obs.N;
    Eigen::MatrixXd K = PHT * S.inverse();

    // Copy X along the diagonals if more than one measurement
    Eigen::MatrixXd BigXinv;
    state_.copyDiagXinv(obs.Y.rows()/state_.dimX(), BigXinv);
    // Compute correction terms
    Eigen::MatrixXd Z = BigXinv*obs.Y - obs.b;
    Eigen::VectorXd delta = K*obs.PI*Z;
    Eigen::MatrixXd dX = Exp_SEK3(delta.segment(0,delta.rows()-state_.dimTheta()));
    Eigen::VectorXd dTheta = delta.segment(delta.rows()-state_.dimTheta(), state_.dimTheta());

    // Update state
    Eigen::MatrixXd X_new = state_.getX()*dX; // Left-Invariant Update
    Eigen::VectorXd Theta_new = state_.getTheta() + dTheta;
    state_.setX(X_new); 
    state_.setTheta(Theta_new);

    // Update Covariance
    Eigen::MatrixXd IKH = Eigen::MatrixXd::Identity(state_.dimP(),state_.dimP()) - K*obs.H;
    Eigen::MatrixXd P_new = IKH * P * IKH.transpose() + K*obs.N*K.transpose(); // Joseph update form
    // For now, the covariance is always assumed to be right-invariant
    // therefore, we need to map it back right-invariant 
    Adj.block(0,0,dimP-dimTheta,dimP-dimTheta) = Adjoint_SEK3(state_.getX()); // TODO: move to analytical inverse
    P_new = (Adj * P_new * Adj.transpose()).eval(); 
    state_.setP(P_new); 
}   


// Correct state using kinematics measured between imu and contact point
void InEKF::CorrectKinematics(const vectorKinematics& measured_kinematics) {
    Eigen::VectorXd Y, b;
    Eigen::MatrixXd H, N, PI;

    Eigen::Matrix3d R = state_.getRotation();
    vector<pair<int,int> > remove_contacts;
    vectorKinematics new_contacts;
    vector<int> used_contact_ids;

   for (vectorKinematicsIterator it=measured_kinematics.begin(); it!=measured_kinematics.end(); ++it) {
        // Detect and skip if an ID is not unique (this would cause singularity issues in InEKF::Correct)
        if (find(used_contact_ids.begin(), used_contact_ids.end(), it->id) != used_contact_ids.end()) { 
            cout << "Duplicate contact ID detected! Skipping measurement.\n";
            continue; 
        } else { used_contact_ids.push_back(it->id); }

        // Find contact indicator for the kinematics measurement
        map<int,bool>::iterator it_contact = contacts_.find(it->id);
        if (it_contact == contacts_.end()) { continue; } // Skip if contact state is unknown
        bool contact_indicated = it_contact->second;

        // See if we can find id estimated_contact_positions
        map<int,int>::iterator it_estimated = estimated_contact_positions_.find(it->id);
        bool found = it_estimated!=estimated_contact_positions_.end();

        // If contact is not indicated and id is found in estimated_contacts_, then remove state
        if (!contact_indicated && found) {
            remove_contacts.push_back(*it_estimated); // Add id to remove list
        //  If contact is indicated and id is not found i n estimated_contacts_, then augment state
        } else if (contact_indicated && !found) {
            new_contacts.push_back(*it); // Add to augment list

        // If contact is indicated and id is found in estimated_contacts_, then correct using kinematics
        } else if (contact_indicated && found) {
            int dimX = state_.dimX();
            int dimTheta = state_.dimTheta();
            int dimP = state_.dimP();
            int startIndex;

            // Fill out Y
            startIndex = Y.rows();
            Y.conservativeResize(startIndex+dimX, Eigen::NoChange);
            Y.segment(startIndex,dimX) = Eigen::VectorXd::Zero(dimX);
            Y.segment(startIndex,3) = it->pose.block<3,1>(0,3); // p_bc
            Y(startIndex+4) = 1; 
            Y(startIndex+it_estimated->second) = -1;       

            // Fill out b
            startIndex = b.rows();
            b.conservativeResize(startIndex+dimX, Eigen::NoChange);
            b.segment(startIndex,dimX) = Eigen::VectorXd::Zero(dimX);
            b(startIndex+4) = 1;       
            b(startIndex+it_estimated->second) = -1;       

            // Fill out H
            startIndex = H.rows();
            H.conservativeResize(startIndex+3, dimP);
            H.block(startIndex,0,3,dimP) = Eigen::MatrixXd::Zero(3,dimP);
            H.block(startIndex,6,3,3) = -Eigen::Matrix3d::Identity(); // -I
            H.block(startIndex,3*it_estimated->second-dimTheta,3,3) = Eigen::Matrix3d::Identity(); // I

            // Fill out N
            startIndex = N.rows();
            N.conservativeResize(startIndex+3, startIndex+3);
            N.block(startIndex,0,3,startIndex) = Eigen::MatrixXd::Zero(3,startIndex);
            N.block(0,startIndex,startIndex,3) = Eigen::MatrixXd::Zero(startIndex,3);
            N.block(startIndex,startIndex,3,3) = R * it->covariance.block<3,3>(3,3) * R.transpose();

            // Fill out PI      
            startIndex = PI.rows();
            int startIndex2 = PI.cols();
            PI.conservativeResize(startIndex+3, startIndex2+dimX);
            PI.block(startIndex,0,3,startIndex2) = Eigen::MatrixXd::Zero(3,startIndex2);
            PI.block(0,startIndex2,startIndex,dimX) = Eigen::MatrixXd::Zero(startIndex,dimX);
            PI.block(startIndex,startIndex2,3,dimX) = Eigen::MatrixXd::Zero(3,dimX);
            PI.block(startIndex,startIndex2,3,3) = Eigen::Matrix3d::Identity();

        //  If contact is not indicated and id is found in estimated_contacts_, then skip
        } else {
            continue;
        }
    }

    // Correct state using stacked observation
    Observation obs(Y,b,H,N,PI);
    if (!obs.empty()) {
        // std::cout << obs << endl; 
        this->CorrectRightInvariant(obs);
    }

    // Remove contacts from state
    if (remove_contacts.size() > 0) {
        Eigen::MatrixXd X_rem = state_.getX(); 
        Eigen::MatrixXd P_rem = state_.getP();
        for (vector<pair<int,int> >::iterator it=remove_contacts.begin(); it!=remove_contacts.end(); ++it) {
            // Remove row and column from X
            removeRowAndColumn(X_rem, it->second);
            // Remove 3 rows and columns from P
            int startIndex = 3 + 3*(it->second-3);
            removeRowAndColumn(P_rem, startIndex); // TODO: Make more efficient
            removeRowAndColumn(P_rem, startIndex); // TODO: Make more efficient
            removeRowAndColumn(P_rem, startIndex); // TODO: Make more efficient
            // Update all indices for estimated_landmarks and estimated_contact_positions
            for (map<int,int>::iterator it2=estimated_landmarks_.begin(); it2!=estimated_landmarks_.end(); ++it2) {
                if (it2->second > it->second) 
                    it2->second -= 1;
            }
            for (map<int,int>::iterator it2=estimated_contact_positions_.begin(); it2!=estimated_contact_positions_.end(); ++it2) {
                if (it2->second > it->second) 
                    it2->second -= 1;
            }
            // We also need to update the indices of remove_contacts in the case where multiple contacts are being removed at once
            for (vector<pair<int,int> >::iterator it2=it; it2!=remove_contacts.end(); ++it2) {
                if (it2->second > it->second) 
                    it2->second -= 1;
            }
            // Remove from list of estimated contact positions 
            estimated_contact_positions_.erase(it->first);
        }
        // Update state and covariance
        state_.setX(X_rem);
        state_.setP(P_rem);
    }


    // Augment state with newly detected contacts
    if (new_contacts.size() > 0) {
        Eigen::MatrixXd X_aug = state_.getX(); 
        Eigen::MatrixXd P_aug = state_.getP();
        Eigen::Vector3d p = state_.getPosition();
        for (vectorKinematicsIterator it=new_contacts.begin(); it!=new_contacts.end(); ++it) {
            // Initialize new landmark mean
            int startIndex = X_aug.rows();
            X_aug.conservativeResize(startIndex+1, startIndex+1);
            X_aug.block(startIndex,0,1,startIndex) = Eigen::MatrixXd::Zero(1,startIndex);
            X_aug.block(0,startIndex,startIndex,1) = Eigen::MatrixXd::Zero(startIndex,1);
            X_aug(startIndex, startIndex) = 1;
            X_aug.block(0,startIndex,3,1) = p + R*it->pose.block<3,1>(0,3);

            // Initialize new landmark covariance - TODO:speed up
            Eigen::MatrixXd F = Eigen::MatrixXd::Zero(state_.dimP()+3,state_.dimP()); 
            F.block(0,0,state_.dimP()-state_.dimTheta(),state_.dimP()-state_.dimTheta()) = Eigen::MatrixXd::Identity(state_.dimP()-state_.dimTheta(),state_.dimP()-state_.dimTheta()); // for old X
            F.block(state_.dimP()-state_.dimTheta(),6,3,3) = Eigen::Matrix3d::Identity(); // for new contact
            F.block(state_.dimP()-state_.dimTheta()+3,state_.dimP()-state_.dimTheta(),state_.dimTheta(),state_.dimTheta()) = Eigen::MatrixXd::Identity(state_.dimTheta(),state_.dimTheta()); // for theta
            Eigen::MatrixXd G = Eigen::MatrixXd::Zero(F.rows(),3);
            G.block(G.rows()-state_.dimTheta()-3,0,3,3) = R;
            P_aug = (F*P_aug*F.transpose() + G*it->covariance.block<3,3>(3,3)*G.transpose()).eval(); 

            // Update state and covariance
            state_.setX(X_aug); // TODO: move outside of loop (need to make loop independent of state_)
            state_.setP(P_aug);

            // Add to list of estimated contact positions
            estimated_contact_positions_.insert(pair<int,int> (it->id, startIndex));
        }
    }
    return; 
}


// Create Observation from vector of landmark measurements
void InEKF::CorrectLandmarks(const vectorLandmarks& measured_landmarks) {
    Eigen::VectorXd Y, b;
    Eigen::MatrixXd H, N, PI;

    Eigen::Matrix3d R = state_.getRotation();
    vectorLandmarks new_landmarks;
    vector<int> used_landmark_ids;
    
    for (vectorLandmarksIterator it=measured_landmarks.begin(); it!=measured_landmarks.end(); ++it) {
        // Detect and skip if an ID is not unique (this would cause singularity issues in InEKF::Correct)
        if (find(used_landmark_ids.begin(), used_landmark_ids.end(), it->id) != used_landmark_ids.end()) { 
            cout << "Duplicate landmark ID detected! Skipping measurement.\n";
            continue; 
        } else { used_landmark_ids.push_back(it->id); }

        // See if we can find id in prior_landmarks or estimated_landmarks
        mapIntVector3dIterator it_prior = prior_landmarks_.find(it->id);
        map<int,int>::iterator it_estimated = estimated_landmarks_.find(it->id);
        if (it_prior!=prior_landmarks_.end()) {
            // Found in prior landmark set
            int dimX = state_.dimX();
            int dimTheta = state_.dimTheta();
            int dimP = state_.dimP();
            int startIndex;

            // Fill out Y
            startIndex = Y.rows();
            Y.conservativeResize(startIndex+dimX, Eigen::NoChange);
            Y.segment(startIndex,dimX) = Eigen::VectorXd::Zero(dimX);
            Y.segment(startIndex,3) = it->position; // p_bl
            Y(startIndex+4) = 1; 

            // Fill out b
            startIndex = b.rows();
            b.conservativeResize(startIndex+dimX, Eigen::NoChange);
            b.segment(startIndex,dimX) = Eigen::VectorXd::Zero(dimX);
            b.segment(startIndex,3) = it_prior->second; // p_wl
            b(startIndex+4) = 1;       

            // Fill out H
            startIndex = H.rows();
            H.conservativeResize(startIndex+3, dimP);
            H.block(startIndex,0,3,dimP) = Eigen::MatrixXd::Zero(3,dimP);
            H.block(startIndex,0,3,3) = skew(it_prior->second); // skew(p_wl)
            H.block(startIndex,6,3,3) = -Eigen::Matrix3d::Identity(); // -I

            // Fill out N
            startIndex = N.rows();
            N.conservativeResize(startIndex+3, startIndex+3);
            N.block(startIndex,0,3,startIndex) = Eigen::MatrixXd::Zero(3,startIndex);
            N.block(0,startIndex,startIndex,3) = Eigen::MatrixXd::Zero(startIndex,3);
            N.block(startIndex,startIndex,3,3) = R * it->covariance * R.transpose();

            // Fill out PI      
            startIndex = PI.rows();
            int startIndex2 = PI.cols();
            PI.conservativeResize(startIndex+3, startIndex2+dimX);
            PI.block(startIndex,0,3,startIndex2) = Eigen::MatrixXd::Zero(3,startIndex2);
            PI.block(0,startIndex2,startIndex,dimX) = Eigen::MatrixXd::Zero(startIndex,dimX);
            PI.block(startIndex,startIndex2,3,dimX) = Eigen::MatrixXd::Zero(3,dimX);
            PI.block(startIndex,startIndex2,3,3) = Eigen::Matrix3d::Identity();

        } else if (it_estimated!=estimated_landmarks_.end()) {;
            // Found in estimated landmark set
            int dimX = state_.dimX();
            int dimTheta = state_.dimTheta();
            int dimP = state_.dimP();
            int startIndex;

            // Fill out Y
            startIndex = Y.rows();
            Y.conservativeResize(startIndex+dimX, Eigen::NoChange);
            Y.segment(startIndex,dimX) = Eigen::VectorXd::Zero(dimX);
            Y.segment(startIndex,3) = it->position; // p_bl
            Y(startIndex+4) = 1; 
            Y(startIndex+it_estimated->second) = -1;       

            // Fill out b
            startIndex = b.rows();
            b.conservativeResize(startIndex+dimX, Eigen::NoChange);
            b.segment(startIndex,dimX) = Eigen::VectorXd::Zero(dimX);
            b(startIndex+4) = 1;       
            b(startIndex+it_estimated->second) = -1;       

            // Fill out H
            startIndex = H.rows();
            H.conservativeResize(startIndex+3, dimP);
            H.block(startIndex,0,3,dimP) = Eigen::MatrixXd::Zero(3,dimP);
            H.block(startIndex,6,3,3) = -Eigen::Matrix3d::Identity(); // -I
            H.block(startIndex,3*it_estimated->second-dimTheta,3,3) = Eigen::Matrix3d::Identity(); // I

            // Fill out N
            startIndex = N.rows();
            N.conservativeResize(startIndex+3, startIndex+3);
            N.block(startIndex,0,3,startIndex) = Eigen::MatrixXd::Zero(3,startIndex);
            N.block(0,startIndex,startIndex,3) = Eigen::MatrixXd::Zero(startIndex,3);
            N.block(startIndex,startIndex,3,3) = R * it->covariance * R.transpose();

            // Fill out PI      
            startIndex = PI.rows();
            int startIndex2 = PI.cols();
            PI.conservativeResize(startIndex+3, startIndex2+dimX);
            PI.block(startIndex,0,3,startIndex2) = Eigen::MatrixXd::Zero(3,startIndex2);
            PI.block(0,startIndex2,startIndex,dimX) = Eigen::MatrixXd::Zero(startIndex,dimX);
            PI.block(startIndex,startIndex2,3,dimX) = Eigen::MatrixXd::Zero(3,dimX);
            PI.block(startIndex,startIndex2,3,3) = Eigen::Matrix3d::Identity();


        } else {
            // First time landmark as been detected (add to list for later state augmentation)
            new_landmarks.push_back(*it);
        }
    }

    // Correct state using stacked observation
    Observation obs(Y,b,H,N,PI);
    if (!obs.empty()) {
        this->CorrectRightInvariant(obs);
    }

    // Augment state with newly detected landmarks
    if (new_landmarks.size() > 0) {
        Eigen::MatrixXd X_aug = state_.getX(); 
        Eigen::MatrixXd P_aug = state_.getP();
        Eigen::Vector3d p = state_.getPosition();
        for (vectorLandmarksIterator it=new_landmarks.begin(); it!=new_landmarks.end(); ++it) {
            // Initialize new landmark mean
            int startIndex = X_aug.rows();
            X_aug.conservativeResize(startIndex+1, startIndex+1);
            X_aug.block(startIndex,0,1,startIndex) = Eigen::MatrixXd::Zero(1,startIndex);
            X_aug.block(0,startIndex,startIndex,1) = Eigen::MatrixXd::Zero(startIndex,1);
            X_aug(startIndex, startIndex) = 1;
            X_aug.block(0,startIndex,3,1) = p + R*it->position;

            // Initialize new landmark covariance - TODO:speed up
            Eigen::MatrixXd F = Eigen::MatrixXd::Zero(state_.dimP()+3,state_.dimP()); 
            F.block(0,0,state_.dimP()-state_.dimTheta(),state_.dimP()-state_.dimTheta()) = Eigen::MatrixXd::Identity(state_.dimP()-state_.dimTheta(),state_.dimP()-state_.dimTheta()); // for old X
            F.block(state_.dimP()-state_.dimTheta(),6,3,3) = Eigen::Matrix3d::Identity(); // for new landmark
            F.block(state_.dimP()-state_.dimTheta()+3,state_.dimP()-state_.dimTheta(),state_.dimTheta(),state_.dimTheta()) = Eigen::MatrixXd::Identity(state_.dimTheta(),state_.dimTheta()); // for theta
            Eigen::MatrixXd G = Eigen::MatrixXd::Zero(F.rows(),3);
            G.block(G.rows()-state_.dimTheta()-3,0,3,3) = R;
            P_aug = (F*P_aug*F.transpose() + G*it->covariance*G.transpose()).eval();

            // Update state and covariance
            state_.setX(X_aug);
            state_.setP(P_aug);

            // Add to list of estimated landmarks
            estimated_landmarks_.insert(pair<int,int> (it->id, startIndex));
        }
    }
    return;    
}


// Remove landmarks by IDs
void InEKF::RemoveLandmarks(const int landmark_id) {
     // Search for landmark in state
    map<int,int>::iterator it = estimated_landmarks_.find(landmark_id);
    if (it!=estimated_landmarks_.end()) {
        // Get current X and P
        Eigen::MatrixXd X_rem = state_.getX(); 
        Eigen::MatrixXd P_rem = state_.getP();
        // Remove row and column from X
        removeRowAndColumn(X_rem, it->second);
        // Remove 3 rows and columns from P
        int startIndex = 3 + 3*(it->second-3);
        removeRowAndColumn(P_rem, startIndex); // TODO: Make more efficient
        removeRowAndColumn(P_rem, startIndex); // TODO: Make more efficient
        removeRowAndColumn(P_rem, startIndex); // TODO: Make more efficient
        // Update all indices for estimated_landmarks and estimated_contact_positions (TODO: speed this up)
        for (map<int,int>::iterator it2=estimated_landmarks_.begin(); it2!=estimated_landmarks_.end(); ++it2) {
            if (it2->second > it->second) 
                it2->second -= 1;
        }
        for (map<int,int>::iterator it2=estimated_contact_positions_.begin(); it2!=estimated_contact_positions_.end(); ++it2) {
            if (it2->second > it->second) 
                it2->second -= 1;
        }
        // Remove from list of estimated landmark positions (after we are done with iterator)
        estimated_landmarks_.erase(it->first);
        // Update state and covariance
        state_.setX(X_rem);
        state_.setP(P_rem);   
    }
}


// Remove landmarks by IDs
void InEKF::RemoveLandmarks(const std::vector<int> landmark_ids) {
    // Loop over landmark_ids and remove
    for (int i=0; i<landmark_ids.size(); ++i) {
        this->RemoveLandmarks(landmark_ids[i]);
    }
}


// Remove prior landmarks by IDs
void InEKF::RemovePriorLandmarks(const int landmark_id) {
    // Search for landmark in state
    mapIntVector3dIterator it = prior_landmarks_.find(landmark_id);
    if (it!=prior_landmarks_.end()) { 
        // Remove from list of estimated landmark positions
        prior_landmarks_.erase(it->first);
    }
}


// Remove prior landmarks by IDs
void InEKF::RemovePriorLandmarks(const std::vector<int> landmark_ids) {
    // Loop over landmark_ids and remove
    for (int i=0; i<landmark_ids.size(); ++i) {
        this->RemovePriorLandmarks(landmark_ids[i]);
    }
}


// Corrects state using magnetometer measurements (Right Invariant)
void InEKF::CorrectMagnetometer(const Eigen::Vector3d& measured_magnetic_field, const Eigen::Matrix3d& covariance) {
    Eigen::VectorXd Y, b;
    Eigen::MatrixXd H, N, PI;

    // Get Rotation Estimate
    Eigen::Matrix3d R = state_.getRotation();

    // Fill out observation data
    int dimX = state_.dimX();
    int dimTheta = state_.dimTheta();
    int dimP = state_.dimP();

    // Fill out Y
    Y.conservativeResize(dimX, Eigen::NoChange);
    Y.segment(0,dimX) = Eigen::VectorXd::Zero(dimX);
    Y.segment<3>(0) = measured_magnetic_field;

    // Fill out b
    b.conservativeResize(dimX, Eigen::NoChange);
    b.segment(0,dimX) = Eigen::VectorXd::Zero(dimX);
    b.segment<3>(0) = magnetic_field_;

    // Fill out H
    H.conservativeResize(3, dimP);
    H.block(0,0,3,dimP) = Eigen::MatrixXd::Zero(3,dimP);
    H.block<3,3>(0,0) = skew(magnetic_field_); 

    // Fill out N
    N.conservativeResize(3, 3);
    N = R * covariance * R.transpose();

    // Fill out PI      
    PI.conservativeResize(3, dimX);
    PI.block(0,0,3,dimX) = Eigen::MatrixXd::Zero(3,dimX);
    PI.block(0,0,3,3) = Eigen::Matrix3d::Identity();
    

    // Correct state using stacked observation
    Observation obs(Y,b,H,N,PI);
    if (!obs.empty()) {
        this->CorrectRightInvariant(obs);
        // cout << obs << endl;
    }
}


// Observation of absolute position - GPS (Left-Invariant Measurement)
void InEKF::CorrectPosition(const Eigen::Vector3d& measured_position, const Eigen::Matrix3d& covariance, const Eigen::Vector3d& indices) {
    Eigen::VectorXd Y, b;
    Eigen::MatrixXd H, N, PI;

    // Fill out observation data
    int dimX = state_.dimX();
    int dimTheta = state_.dimTheta();
    int dimP = state_.dimP();

    // Fill out Y
    Y.conservativeResize(dimX, Eigen::NoChange);
    Y.segment(0,dimX) = Eigen::VectorXd::Zero(dimX);
    Y.segment<3>(0) = measured_position;
    Y(4) = 1;       

    // Fill out b
    b.conservativeResize(dimX, Eigen::NoChange);
    b.segment(0,dimX) = Eigen::VectorXd::Zero(dimX);
    b(4) = 1;       

    // Fill out H
    H.conservativeResize(3, dimP);
    H.block(0,0,3,dimP) = Eigen::MatrixXd::Zero(3,dimP);
    H.block<3,3>(0,6) = Eigen::Matrix3d::Identity(); 

    // Fill out N
    N.conservativeResize(3, 3);
    N = covariance;

    // Fill out PI      
    PI.conservativeResize(3, dimX);
    PI.block(0,0,3,dimX) = Eigen::MatrixXd::Zero(3,dimX);
    PI.block(0,0,3,3) = Eigen::Matrix3d::Identity();

    // Modify measurement based on chosen indices
    const double HIGH_UNCERTAINTY = 1e6;
    Eigen::Vector3d p = state_.getPosition();
    if (!indices(0)) { 
        Y(0) = p(0);
        N(0,0) = HIGH_UNCERTAINTY;
        N(0,1) = 0;
        N(0,2) = 0;
        N(1,0) = 0;
        N(2,0) = 0;
        } 
    if (!indices(1)) { 
        Y(1) = p(1);
        N(1,0) = 0;
        N(1,1) = HIGH_UNCERTAINTY;
        N(1,2) = 0;
        N(0,1) = 0;
        N(2,1) = 0;
        } 
    if (!indices(2)) { 
        Y(2) = p(2);
        N(2,0) = 0;
        N(2,1) = 0;
        N(2,2) = HIGH_UNCERTAINTY;
        N(0,2) = 0;
        N(1,2) = 0;
        } 

    // Correct state using stacked observation
    Observation obs(Y,b,H,N,PI);
    if (!obs.empty()) {
        this->CorrectLeftInvariant(obs);
        // cout << obs << endl;
    }
}


// Observation of absolute z-position of contact points (Left-Invariant Measurement)
void InEKF::CorrectContactPosition(const int id, const Eigen::Vector3d& measured_contact_position, const Eigen::Matrix3d& covariance, const Eigen::Vector3d& indices) {
    Eigen::VectorXd Y, b;
    Eigen::MatrixXd H, N, PI;
    
    // See if we can find id estimated_contact_positions
    map<int,int>::iterator it_estimated = estimated_contact_positions_.find(id);
    if (it_estimated!=estimated_contact_positions_.end()) { 
        // Fill out observation data
        int dimX = state_.dimX();
        int dimTheta = state_.dimTheta();
        int dimP = state_.dimP();
        int startIndex;

        // Fill out Y
        Y.conservativeResize(dimX, Eigen::NoChange);
        Y.segment(0,dimX) = Eigen::VectorXd::Zero(dimX);
        Y.segment<3>(0) = measured_contact_position;
        Y(it_estimated->second) = 1;       

        // Fill out b
        b.conservativeResize(dimX, Eigen::NoChange);
        b.segment(0,dimX) = Eigen::VectorXd::Zero(dimX);
        b(it_estimated->second) = 1;       

        // Fill out H
        H.conservativeResize(3, dimP);
        H.block(0,0,3,dimP) = Eigen::MatrixXd::Zero(3,dimP);
        H.block<3,3>(0,3*it_estimated->second-dimTheta) = Eigen::Matrix3d::Identity(); 

        // Fill out N
        N.conservativeResize(3, 3);
        N.block<3,3>(0,0) = covariance;

        // Fill out PI      
        PI.conservativeResize(3, dimX);
        PI.block(0,0,3,dimX) = Eigen::MatrixXd::Zero(3,dimX);
        PI.block(0,0,3,3) = Eigen::Matrix3d::Identity();

        // Modify measurement based on chosen indices
        Eigen::MatrixXd X = state_.getX();
        const double HIGH_UNCERTAINTY = 1e6;
        if (!indices(0)) { 
            Y(0) = X(0,it_estimated->second);
            N(0,0) = HIGH_UNCERTAINTY;
            N(0,1) = 0;
            N(0,2) = 0;
            N(1,0) = 0;
            N(2,0) = 0;
         } 
        if (!indices(1)) { 
            Y(1) = X(1,it_estimated->second);
            N(1,0) = 0;
            N(1,1) = HIGH_UNCERTAINTY;
            N(1,2) = 0;
            N(0,1) = 0;
            N(2,1) = 0;
         } 
        if (!indices(2)) { 
            Y(2) = X(2,it_estimated->second);
            N(2,0) = 0;
            N(2,1) = 0;
            N(2,2) = HIGH_UNCERTAINTY;
            N(0,2) = 0;
            N(1,2) = 0;
         } 
    }

    // Correct state using stacked observation
    Observation obs(Y,b,H,N,PI);
    if (!obs.empty()) {
        this->CorrectLeftInvariant(obs);
        // cout << obs << endl;
    }

}


void removeRowAndColumn(Eigen::MatrixXd& M, int index) {
    unsigned int dimX = M.cols();
    // cout << "Removing index: " << index<< endl;
    M.block(index,0,dimX-index-1,dimX) = M.bottomRows(dimX-index-1).eval();
    M.block(0,index,dimX,dimX-index-1) = M.rightCols(dimX-index-1).eval();
    M.conservativeResize(dimX-1,dimX-1);
}

} // end inekf namespace
