/*=========================================================================

Program:   NeuroLib (DTI command line tools)
Language:  C++
Date:      $Date: 2009/08/03 17:36:42 $
Version:   $Revision: 1.8 $
Author:    Casey Goodlett (gcasey@sci.utah.edu)

Copyright (c)  Casey Goodlett. All rights reserved.
See NeuroLibCopyright.txt or http://www.ia.unc.edu/dev/Copyright.htm for details.

This software is distributed WITHOUT ANY WARRANTY; without even
the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
// STL includes
#include <string>
#include <iostream>
#include <fstream>

// ITK includes
#include <itkDiffusionTensor3D.h>
#include <itkImageFileReader.h>
#include <itkImageFileWriter.h>
#include <itkTensorLinearInterpolateImageFunction.h>
#include <itkVectorLinearInterpolateImageFunction.h>
#include <itkVersion.h>

//#include "FiberCalculator.h"
#include "deformationfieldoperations.h"
#include "fiberio.h"
#include "dtitypes.h"
#include "fiberprocessCLP.h"

int main(int argc, char* argv[])
{
  PARSE_ARGS;

  if(fiberFile == "")
    {
      std::cerr << "A fiber file has to be specified" << std::endl;
      return EXIT_FAILURE;
    }
  const bool VERBOSE = verbose;
  
  // Reader fiber bundle
  GroupType::Pointer group = readFiberFile(fiberFile);
  
  DeformationImageType::Pointer deformationfield(NULL);
  if(hField != "")
    deformationfield = readDeformationField(hField, HField);
  else if(displacementField != "")
    deformationfield = readDeformationField(displacementField, Displacement);
  else
    deformationfield = NULL;
  
  typedef itk::VectorLinearInterpolateImageFunction<DeformationImageType, double> DeformationInterpolateType;
  DeformationInterpolateType::Pointer definterp(NULL);
  if(deformationfield)
  {
    definterp = DeformationInterpolateType::New();
    definterp->SetInputImage(deformationfield);
  } else {
    noWarp = true;
  }
  
  // Setup new fiber bundle group
  GroupType::Pointer newgroup = GroupType::New();
  newgroup->SetId(0);
  
  ChildrenListType* children = group->GetChildren(0);
  
  if(VERBOSE)
    std::cout << "Getting spacing" << std::endl;
  
  // Get Spacing and offset from group
  double spacing[3];
  if( noWarp )
  {
    newgroup->SetObjectToWorldTransform( group->GetObjectToWorldTransform() ) ;
    newgroup->ComputeObjectToParentTransform() ;
    for(unsigned int i =0; i < 3; i++)
    {
      spacing[i] = (group->GetSpacing())[i];
    }
  }
  else
  {
    spacing[0] = spacing[1] = spacing[2] = 1; 
  }
  newgroup->SetSpacing(spacing);
  
  itk::Vector<double, 3> sooffset;
  for(unsigned int i = 0; i < 3; i++)
    sooffset[i] = (group->GetObjectToParentTransform()->GetOffset())[i];
  
  if(VERBOSE)
  {
    std::cout << "Group Spacing: " << spacing[0] << ", " << spacing[1] << ", " << spacing[2] << std::endl;
    std::cout << "Group Offset: " << sooffset[0] << ", " << sooffset[1]  << ", " << sooffset[2] << std::endl;
    if (deformationfield)
      std::cout << "deformationfield: '" << deformationfield << "'" << std::endl; 
  }
  
  // Setup tensor file if available
  typedef itk::ImageFileReader<TensorImageType> TensorImageReader;
  typedef itk::TensorLinearInterpolateImageFunction<TensorImageType, double> TensorInterpolateType;
  TensorImageReader::Pointer tensorreader = NULL;
  TensorInterpolateType::Pointer tensorinterp = NULL;
  
  if(tensorVolume != "")
  {
    tensorreader = TensorImageReader::New();
    tensorinterp = TensorInterpolateType::New();
    
    tensorreader->SetFileName(tensorVolume);
    try
    {
      tensorreader->Update();
      tensorinterp->SetInputImage(tensorreader->GetOutput());
    }
    catch(itk::ExceptionObject exp)
    {
      std::cerr << exp << std::endl;
      return EXIT_FAILURE;
    }
  }
  
  if(VERBOSE)
    std::cout << "Starting Loop" << std::endl;
  
  ChildrenListType::iterator it;
  unsigned int id = 1;
  
  // Need to allocate an image to write into for creating
  // the fiber label map
  IntImageType::Pointer labelimage;
  if(voxelize != "")
  {
    if(tensorVolume == "")
    {
      std::cerr << "Must specify tensor file to copy image metadata for fiber voxelize." << std::endl;
      return EXIT_FAILURE;
    }
    //tensorreader->GetOutput();
    labelimage = IntImageType::New();
    labelimage->SetSpacing(tensorreader->GetOutput()->GetSpacing());
    labelimage->SetOrigin(tensorreader->GetOutput()->GetOrigin());
    labelimage->SetDirection(tensorreader->GetOutput()->GetDirection());
    labelimage->SetRegions(tensorreader->GetOutput()->GetLargestPossibleRegion());
    labelimage->Allocate();
    labelimage->FillBuffer(0);
  }
  
  // For each fiber
  for(it = children->begin(); it != children->end(); it++)
  {
    DTIPointListType pointlist = dynamic_cast<DTITubeType*>((*it).GetPointer())->GetPoints();
    DTITubeType::Pointer newtube = DTITubeType::New();
    DTIPointListType newpoints;
    
    DTIPointListType::iterator pit;
    
    typedef DeformationInterpolateType::ContinuousIndexType ContinuousIndexType;
    ContinuousIndexType tensor_ci, def_ci ;
    // For each point along the fiber
//std::cout<<"plop"<<std::endl;
    for(pit = pointlist.begin(); pit != pointlist.end(); ++pit)
    {
      typedef DTIPointType::PointType PointType;
//      std::cout<<"plop3"<<std::endl;
      // p is not really a point its a continuous index
      const PointType p = pit->GetPosition();
      DTITubeType::TransformPointer transform = ((*it).GetPointer())->GetObjectToWorldTransform() ;
      const PointType p_world_orig = transform->TransformPoint( p ) ;
//      std::cout<<p_world_orig<<" "<<p<<std::endl;
      itk::Point<double, 3> pt_trans = p_world_orig ;
      if(deformationfield)
      {
        deformationfield->TransformPhysicalPointToContinuousIndex(p_world_orig, def_ci);

        if( !deformationfield->GetLargestPossibleRegion().IsInside( def_ci ) )
        {
          std::cerr << "Fiber is outside deformation field image. Deformation field has to be in the fiber space. Warning: Original position will be used" << std::endl ;
        }
        else
        {
          DeformationPixelType warp(definterp->EvaluateAtContinuousIndex(def_ci).GetDataPointer());
          for( int i = 0 ; i < 3 ; i++ )
          {
            pt_trans[ i ] += warp[ i ] ;
          }
//          deformationfield->TransformContinuousIndexToPhysicalPoint( def_ci , pt_trans ) ;
//          std::cout<<p << " "<< p_world_orig<<" "<<pt_trans<<std::endl;
        }
      }
//            std::cout<<"plop6"<<std::endl;
      
      if(voxelize != "")
      {
//        std::cout<<p << " "<< p_world_orig<<" "<<pt_trans<<std::endl;
        ContinuousIndexType cind;
        itk::Index<3> ind;
        labelimage->TransformPhysicalPointToContinuousIndex(pt_trans, cind);
        ind[0] = static_cast<long int>(vnl_math_rnd_halfinttoeven(cind[0]));
        ind[1] = static_cast<long int>(vnl_math_rnd_halfinttoeven(cind[1]));
        ind[2] = static_cast<long int>(vnl_math_rnd_halfinttoeven(cind[2]));
	
        if(!labelimage->GetLargestPossibleRegion().IsInside(ind))
        {
          std::cerr << "Error index: " << ind << " not in image"  << std::endl;
          std::cout << "Ignoring" << std::endl;
          //return EXIT_FAILURE;
	}
        if(voxelizeCountFibers)
        {
          labelimage->SetPixel(ind, labelimage->GetPixel(ind) + 1);
	}
        else
        {
          labelimage->SetPixel(ind, voxelLabel);
	}
      }
    
      
      DTIPointType newpoint;
      if(noDataChange==true)
	newpoint = *pit;
      // Should not have to do this
      if(noWarp)
      {
//        std::cout<<"no warp"<<std::endl;
	newpoint.SetPosition(p);
      }
      else
      {
	//set the point to world coordinate system and set the spacing to 1
//        std::cout<<"warp"<<std::endl;
	newpoint.SetPosition(pt_trans);
      }
        
      
      
      //newpoint.SetRadius(.4);
      //newpoint.SetRed(0.0);
      //newpoint.SetGreen(1.0);
      //newpoint.SetBlue(0.0);
      
      // Attribute tensor data if provided
      if(tensorVolume != "" && fiberOutput != "" && !noDataChange)
      {
	tensorreader->GetOutput()->TransformPhysicalPointToContinuousIndex(pt_trans, tensor_ci);
	itk::DiffusionTensor3D<double> tensor(tensorinterp->EvaluateAtContinuousIndex(tensor_ci).GetDataPointer());
	
	// TODO: Change SpatialObject interface to accept DiffusionTensor3D
	float sotensor[6];
	for(unsigned int i = 0; i < 6; ++i)
	  sotensor[i] = tensor[i];
	
	typedef itk::DiffusionTensor3D<double>::EigenValuesArrayType EigenValuesType;
	EigenValuesType eigenvalues;
	tensor.ComputeEigenValues(eigenvalues);
      
	newpoint.SetRadius(0.5);
	newpoint.SetTensorMatrix(sotensor);
	newpoint.AddField(itk::DTITubeSpatialObjectPoint<3>::FA, tensor.GetFractionalAnisotropy());
	newpoint.AddField("md", tensor.GetTrace()/3);
	newpoint.AddField("fro", sqrt(tensor[0]*tensor[0] +
				      2*tensor[1]*tensor[1] +
				      2*tensor[2]*tensor[2] +
				      tensor[3]*tensor[3] +
				      2*tensor[4]*tensor[4] +
				      tensor[5]*tensor[5]));
	newpoint.AddField("l1", eigenvalues[2]);
	newpoint.AddField("l2", eigenvalues[1]);
	newpoint.AddField("l3", eigenvalues[0]);
      }
      
      newpoints.push_back(newpoint);
    } 
    newtube->SetSpacing(spacing);
    newtube->SetId(id++);
    newtube->SetPoints(newpoints);
    newgroup->AddSpatialObject(newtube);
  }
//  std::cout<<"plop2"<<std::endl;
  
  if(VERBOSE)
    std::cout << "Ending Loop" << std::endl;
  
  if(VERBOSE)
    std::cout << "Output: " << fiberOutput << std::endl;
  
  if(fiberOutput != "")
    writeFiberFile(fiberOutput, newgroup);
  
  if(voxelize != "")
  {
    typedef itk::ImageFileWriter<IntImageType> LabelWriter;
    LabelWriter::Pointer writer = LabelWriter::New();
    writer->SetInput(labelimage);
    writer->SetFileName(voxelize);
    writer->UseCompressionOn();
    try
    {
      writer->Update();
    }
    catch(itk::ExceptionObject & e)
    {
      std::cerr << e.what() << std::endl;
      return EXIT_FAILURE;
    }
  }

  delete children;
  return EXIT_SUCCESS;
}