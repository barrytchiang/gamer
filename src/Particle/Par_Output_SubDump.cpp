#include "GAMER.h"

#ifdef PARTICLE

// forward declarations
#ifdef TRACER
static void Par_Output_SubTracer_Internal( const char *FileName, const int SubDumpID );
#endif
static void Par_Output_SubParticle_Internal( const char *FileName, const int SubDumpID );


//-------------------------------------------------------------------------------------------------------
// Function    :  Par_Output_SubTracer
// Description :  Write all active tracer particles to a compact HDF5 file SubTracer_%06d
//
// Note        :  1. Automatically includes ALL float/int particle attributes via ParAttFltLabel[]/ParAttIntLabel[]
//                   and all mesh-sampled fields via amr->Par->Mesh_Attr_Label[] (from Input__Par_Mesh).
//                   No test-problem-specific code is required.
//                2. Only PTYPE_TRACER particles are written; massive/inactive particles are excluded.
//                3. Sequential rank-by-rank HDF5 writes; no parallel HDF5 required.
//                4. Invoked by Output_DumpData() when OPT__OUTPUT_SUBDIV_TRACER is enabled.
//-------------------------------------------------------------------------------------------------------
#ifdef TRACER
void Par_Output_SubTracer( const int SubDumpID )
{
#  ifndef SUPPORT_HDF5
   Aux_Error( ERROR_INFO, "Par_Output_SubTracer requires SUPPORT_HDF5 !!\n" );
#  else

   char FileName[2*MAX_STRING];
   sprintf( FileName, "%s/SubTracer_%06d", OUTPUT_DIR, SubDumpID );

   if ( MPI_Rank == 0 )   Aux_Message( stdout, "   Writing \"%s\" ...\n", FileName );

   Par_Output_SubTracer_Internal( FileName, SubDumpID );

   if ( MPI_Rank == 0 )   Aux_Message( stdout, "   Writing \"%s\" ... done\n", FileName );

#  endif // #ifndef SUPPORT_HDF5 ... else
} // FUNCTION : Par_Output_SubTracer
#endif // #ifdef TRACER


//-------------------------------------------------------------------------------------------------------
// Function    :  Par_Output_SubParticle
// Description :  Write all active massive (non-tracer) particles to a compact HDF5 file SubParticle_%06d
//
// Note        :  Same structure as Par_Output_SubTracer but selects PTYPE != PTYPE_TRACER.
//-------------------------------------------------------------------------------------------------------
void Par_Output_SubParticle( const int SubDumpID )
{
#  ifndef SUPPORT_HDF5
   Aux_Error( ERROR_INFO, "Par_Output_SubParticle requires SUPPORT_HDF5 !!\n" );
#  else

   char FileName[2*MAX_STRING];
   sprintf( FileName, "%s/SubParticle_%06d", OUTPUT_DIR, SubDumpID );

   if ( MPI_Rank == 0 )   Aux_Message( stdout, "   Writing \"%s\" ...\n", FileName );

   Par_Output_SubParticle_Internal( FileName, SubDumpID );

   if ( MPI_Rank == 0 )   Aux_Message( stdout, "   Writing \"%s\" ... done\n", FileName );

#  endif // #ifndef SUPPORT_HDF5 ... else
} // FUNCTION : Par_Output_SubParticle


#ifdef SUPPORT_HDF5

//-------------------------------------------------------------------------------------------------------
// Helper : write one category (tracer or massive) of particles to FileName
//          IsTracer=true  → only PTYPE_TRACER
//          IsTracer=false → only non-tracer active particles
//-------------------------------------------------------------------------------------------------------
static void Par_Output_SubDump_HDF5( const char *FileName, const bool IsTracer, const int SubDumpID )
{
   const long   NTotal      = amr->Par->NPar_AcPlusInac;
   const int    NMeshAttr   = amr->Par->Mesh_Attr_Num;
   const bool   ForceFloat32 = IsTracer ? (OPT__OUTPUT_SUBDIV_TRACER == 2) : (OPT__OUTPUT_SUBDIV_PAR == 2);
   const hid_t  H5T_FltOut  = ForceFloat32 ? H5T_NATIVE_FLOAT : H5T_GAMER_REAL_PAR;

// populate mesh-sampled attributes before counting particles
   if ( IsTracer  &&  NMeshAttr > 0 )   Par_Output_TracerParticle_Mesh();

// -------------------------------------------------------
// 1. count target particles on this rank
// -------------------------------------------------------
   long NParThisRank = 0;
   for (long p=0; p<NTotal; p++)
   {
      if ( amr->Par->Mass[p] < (real_par)0.0 )   continue;  // inactive
      const bool isTracer = ( amr->Par->Type[p] == PTYPE_TRACER );
      if ( IsTracer == isTracer )   NParThisRank++;
   }

// gather global count and this rank's write offset
   long *NParEachRank = new long [MPI_NRank];
   MPI_Allgather( &NParThisRank, 1, MPI_LONG, NParEachRank, 1, MPI_LONG, MPI_COMM_WORLD );

   long NParAllRank = 0, NParOffset = 0;
   for (int r=0; r<MPI_NRank; r++)   NParAllRank += NParEachRank[r];
   for (int r=0; r<MPI_Rank;  r++)   NParOffset  += NParEachRank[r];
   delete [] NParEachRank;

// -------------------------------------------------------
// 2. fill local float/int attribute buffers
// -------------------------------------------------------
   real_par **BufFlt = new real_par* [PAR_NATT_FLT_TOTAL];
   long_par **BufInt = new long_par* [PAR_NATT_INT_TOTAL];
   for (int v=0; v<PAR_NATT_FLT_TOTAL; v++)   BufFlt[v] = new real_par [NParThisRank];
   for (int v=0; v<PAR_NATT_INT_TOTAL; v++)   BufInt[v] = new long_par [NParThisRank];

   real_par **BufMesh = NULL;
   if ( IsTracer  &&  NMeshAttr > 0 )
   {
      BufMesh = new real_par* [NMeshAttr];
      for (int v=0; v<NMeshAttr; v++)   BufMesh[v] = new real_par [NParThisRank];
   }

   long idx = 0;
   for (long p=0; p<NTotal; p++)
   {
      if ( amr->Par->Mass[p] < (real_par)0.0 )   continue;
      const bool isTracer = ( amr->Par->Type[p] == PTYPE_TRACER );
      if ( IsTracer != isTracer )   continue;

      for (int v=0; v<PAR_NATT_FLT_TOTAL; v++)   BufFlt[v][idx] = amr->Par->AttributeFlt[v][p];
      for (int v=0; v<PAR_NATT_INT_TOTAL; v++)   BufInt[v][idx] = amr->Par->AttributeInt[v][p];
      if ( BufMesh != NULL )
         for (int v=0; v<NMeshAttr; v++)          BufMesh[v][idx] = amr->Par->Mesh_Attr[v][p];
      idx++;
   }

// -------------------------------------------------------
// 3. root creates HDF5 file, metadata, and empty datasets
// -------------------------------------------------------
   const hsize_t H5_Total = (hsize_t)NParAllRank;

   if ( MPI_Rank == 0 )
   {
      hid_t FileID = H5Fcreate( FileName, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT );
      if ( FileID < 0 )   Aux_Error( ERROR_INFO, "failed to create \"%s\" !!\n", FileName );

      hid_t ScalarSID = H5Screate( H5S_SCALAR );
      hid_t AttrID;
      AttrID = H5Acreate( FileID, "Time",   H5T_NATIVE_DOUBLE, ScalarSID, H5P_DEFAULT, H5P_DEFAULT );
      H5Awrite( AttrID, H5T_NATIVE_DOUBLE, &Time[0] );   H5Aclose( AttrID );
      AttrID = H5Acreate( FileID, "Step",   H5T_NATIVE_LONG,   ScalarSID, H5P_DEFAULT, H5P_DEFAULT );
      H5Awrite( AttrID, H5T_NATIVE_LONG,   &Step );       H5Aclose( AttrID );
      AttrID = H5Acreate( FileID, "SubDumpID", H5T_NATIVE_INT, ScalarSID, H5P_DEFAULT, H5P_DEFAULT );
      H5Awrite( AttrID, H5T_NATIVE_INT,    &SubDumpID );  H5Aclose( AttrID );
      H5Sclose( ScalarSID );

      hid_t GrpID   = H5Gcreate( FileID, "Particle", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT );
      hid_t SpaceID = H5Screate_simple( 1, &H5_Total, NULL );

      for (int v=0; v<PAR_NATT_FLT_TOTAL; v++)
      {
         if ( IsTracer  &&  v == Idx_ParMass )   continue;
         hid_t DID = H5Dcreate( GrpID, ParAttFltLabel[v], H5T_FltOut, SpaceID,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT );
         H5Dclose( DID );
      }
      for (int v=0; v<PAR_NATT_INT_TOTAL; v++)
      {
         hid_t DID = H5Dcreate( GrpID, ParAttIntLabel[v], H5T_GAMER_LONG_PAR, SpaceID,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT );
         H5Dclose( DID );
      }
      if ( IsTracer )
         for (int v=0; v<NMeshAttr; v++)
         {
            hid_t DID = H5Dcreate( GrpID, amr->Par->Mesh_Attr_Label[v], H5T_FltOut, SpaceID,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT );
            H5Dclose( DID );
         }

      H5Sclose( SpaceID );
      H5Gclose( GrpID );
      H5Fclose( FileID );
   }
   MPI_Barrier( MPI_COMM_WORLD );

// -------------------------------------------------------
// 4. sequential rank-by-rank hyperslab writes
// -------------------------------------------------------
   for (int TRank=0; TRank<MPI_NRank; TRank++)
   {
      if ( MPI_Rank == TRank  &&  NParThisRank > 0 )
      {
         SyncHDF5File( FileName );
         hid_t FileID  = H5Fopen( FileName, H5F_ACC_RDWR, H5P_DEFAULT );
         if ( FileID < 0 )   Aux_Error( ERROR_INFO, "rank %d failed to open \"%s\" !!\n", MPI_Rank, FileName );
         hid_t GrpID   = H5Gopen( FileID, "Particle", H5P_DEFAULT );

         const hsize_t Count  = (hsize_t)NParThisRank;
         const hsize_t Offset = (hsize_t)NParOffset;
         hid_t MemSID  = H5Screate_simple( 1, &Count,    NULL );
         hid_t FileSID = H5Screate_simple( 1, &H5_Total, NULL );
         H5Sselect_hyperslab( FileSID, H5S_SELECT_SET, &Offset, NULL, &Count, NULL );

         for (int v=0; v<PAR_NATT_FLT_TOTAL; v++)
         {
            if ( IsTracer  &&  v == Idx_ParMass )   continue;
            hid_t DID = H5Dopen( GrpID, ParAttFltLabel[v], H5P_DEFAULT );
            H5Dwrite( DID, H5T_GAMER_REAL_PAR, MemSID, FileSID, H5P_DEFAULT, BufFlt[v] );
            H5Dclose( DID );
         }
         for (int v=0; v<PAR_NATT_INT_TOTAL; v++)
         {
            hid_t DID = H5Dopen( GrpID, ParAttIntLabel[v], H5P_DEFAULT );
            H5Dwrite( DID, H5T_GAMER_LONG_PAR, MemSID, FileSID, H5P_DEFAULT, BufInt[v] );
            H5Dclose( DID );
         }
         if ( IsTracer  &&  BufMesh != NULL )
            for (int v=0; v<NMeshAttr; v++)
            {
               hid_t DID = H5Dopen( GrpID, amr->Par->Mesh_Attr_Label[v], H5P_DEFAULT );
               H5Dwrite( DID, H5T_GAMER_REAL_PAR, MemSID, FileSID, H5P_DEFAULT, BufMesh[v] );
               H5Dclose( DID );
            }

         H5Sclose( MemSID );
         H5Sclose( FileSID );
         H5Gclose( GrpID );
         H5Fclose( FileID );
      }
      MPI_Barrier( MPI_COMM_WORLD );
   }

// -------------------------------------------------------
// 5. free buffers
// -------------------------------------------------------
   for (int v=0; v<PAR_NATT_FLT_TOTAL; v++)   delete [] BufFlt[v];
   for (int v=0; v<PAR_NATT_INT_TOTAL; v++)   delete [] BufInt[v];
   delete [] BufFlt;
   delete [] BufInt;
   if ( BufMesh != NULL )
   {
      for (int v=0; v<NMeshAttr; v++)   delete [] BufMesh[v];
      delete [] BufMesh;
   }

} // FUNCTION : Par_Output_SubDump_HDF5


#ifdef TRACER
static void Par_Output_SubTracer_Internal( const char *FileName, const int SubDumpID )
{
   Par_Output_SubDump_HDF5( FileName, true, SubDumpID );
}
#endif

static void Par_Output_SubParticle_Internal( const char *FileName, const int SubDumpID )
{
   Par_Output_SubDump_HDF5( FileName, false, SubDumpID );
}

#endif // #ifdef SUPPORT_HDF5


#endif // #ifdef PARTICLE
