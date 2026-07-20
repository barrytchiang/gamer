#include "GAMER.h"


//-------------------------------------------------------------------------------------------------------
// Function    :  Init_SubGrid_Fields
// Description :  Populate SubGridField[] and SubGridField_Num from Input__Sub_Grid
//
// Note        :  1. If Input__Sub_Grid is absent, all NCOMP_TOTAL fluid fields are enabled (default).
//                2. Each non-comment line must contain exactly one FieldLabel[v] string (case-sensitive).
//                3. Called by Init_GAMER() when OPT__OUTPUT_SUBDIV_GRID is true.
//-------------------------------------------------------------------------------------------------------
void Init_SubGrid_Fields()
{

// initialize
   for (int v=0; v<NCOMP_TOTAL; v++)   SubGridField[v] = false;
   SubGridField_Num = 0;

   char FileName[] = "Input__Sub_Grid";
   FILE *File      = fopen( FileName, "r" );

// default: all fields enabled when the file is absent
   if ( File == NULL )
   {
      for (int v=0; v<NCOMP_TOTAL; v++)   SubGridField[v] = true;
      SubGridField_Num = NCOMP_TOTAL;
      return;
   }

   char *Line      = new char [MAX_STRING];
   char  FirstItem[MAX_STRING];

   while ( fgets(Line, MAX_STRING, File) != NULL )
   {
      if ( sscanf(Line, "%s", FirstItem) <= 0  ||  FirstItem[0] == '#' )   continue;

      bool found = false;
      for (int v=0; v<NCOMP_TOTAL; v++)
      {
         if ( strcmp(FieldLabel[v], FirstItem) == 0 )
         {
            SubGridField[v] = true;
            SubGridField_Num++;
            found = true;
            break;
         }
      }

      if ( !found )
         Aux_Error( ERROR_INFO, "unknown field label \"%s\" in %s !!\n", FirstItem, FileName );
   }

   fclose( File );
   delete [] Line;

   if ( SubGridField_Num == 0 )
      Aux_Error( ERROR_INFO, "%s contains no valid field labels !!\n", FileName );

} // FUNCTION : Init_SubGrid_Fields
