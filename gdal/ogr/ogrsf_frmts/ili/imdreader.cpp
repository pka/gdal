/******************************************************************************
 * $Id$
 *
 * Project:  Interlis 1/2 Translator
 * Purpose:  IlisMeta model reader.
 * Author:   Pirmin Kalberer, Sourcepole AG
 *
 ******************************************************************************
 * Copyright (c) 2014, Pirmin Kalberer, Sourcepole AG
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/


#include "imdreader.h"
#include "cpl_minixml.h"
#include <map>
#include <set>
#include <vector>


CPL_CVSID("$Id$");


typedef std::map<CPLString,CPLXMLNode*> StrNodeMap;
typedef std::vector<CPLXMLNode*> NodeVector;

class IliClass
{
public:
    OGRFeatureDefn* poTableDefn;
    NodeVector oFields;
    bool isAssocClass;
    bool hasDerivedClasses;

    IliClass(OGRFeatureDefn* poTableDefnIn) : poTableDefn(poTableDefnIn), oFields(), isAssocClass(false), hasDerivedClasses(false)
    {
    };
    ~IliClass()
    {
        delete poTableDefn;
    };
    void AddFieldNode(CPLXMLNode* node, int iOrderPos)
    {
        if (iOrderPos >= (int)oFields.size())
            oFields.resize(iOrderPos+1);
        //CPLDebug( "OGR_ILI", "Register field with OrderPos %d to Class %s", iOrderPos, poTableDefn->GetName());
        oFields[iOrderPos] = node;
    }
    void AddRoleNode(CPLXMLNode* node, int iOrderPos)
    {
        isAssocClass = true;
        AddFieldNode(node, iOrderPos);
    }
    bool isEmbedded()
    {
        if (isAssocClass)
            for (NodeVector::const_iterator it = oFields.begin(); it != oFields.end(); ++it)
            {
                if (*it == NULL) continue;
                if (CSLTestBoolean(CPLGetXMLValue( *it, "EmbeddedTransfer", "FALSE" )))
                    return true;
            }
        return false;
    }
    OGRwkbGeometryType AddCoord(int iliVersion, const char* psName, CPLXMLNode* psTypeNode) {
      unsigned int dim = 2; //FIXME
      if (iliVersion == 1)
      {
          for (unsigned int i=0; i<dim; i++) {
            OGRFieldDefn fieldDef(CPLSPrintf("%s_%d", psName, i), OFTReal);
            poTableDefn->AddFieldDefn(&fieldDef);
          } 
      }
      OGRwkbGeometryType geomType = (dim > 2) ? wkbPoint25D : wkbPoint;
      return geomType;
    }
    void AddFieldDefinitions(int iliVersion, StrNodeMap& oTidLookup)
    {
        // Delete default geometry field
        poTableDefn->DeleteGeomFieldDefn(0);
        // add TID field
        const char* psTidColName = (iliVersion == 1) ? "_TID" : "TID";
        OGRFieldDefn ofieldDefn(psTidColName, OFTString);
        poTableDefn->AddFieldDefn(&ofieldDefn);
        for (NodeVector::const_iterator it = oFields.begin(); it != oFields.end(); ++it)
        {
            if (*it == NULL) continue;
            const char* psName = CPLGetXMLValue( *it, "Name", NULL );
            const char* psTypeRef = CPLGetXMLValue( *it, "Type.REF", NULL );
            OGRFieldType fieldType = OFTString;
            OGRwkbGeometryType geomType = wkbUnknown;
            if (psTypeRef == NULL) //Assoc Role
                fieldType = OFTString; //FIXME: numeric?
            else
            {
                CPLXMLNode* psElementNode = oTidLookup[psTypeRef];
                const char* typeName = psElementNode->pszValue;
                if (EQUAL(typeName, "IlisMeta07.ModelData.TextType"))
                { //Kind Text,MText
                    fieldType = OFTString;
                }
                else if (EQUAL(typeName, "IlisMeta07.ModelData.EnumType"))
                {
                    fieldType = (iliVersion == 1) ? OFTInteger : OFTString;
                }
                else if (EQUAL(typeName, "IlisMeta07.ModelData.BooleanType"))
                {
                    fieldType = OFTString; //??
                }
                else if (EQUAL(typeName, "IlisMeta07.ModelData.NumType"))
                { //// Unit INTERLIS.ANYUNIT, INTERLIS.TIME, INTERLIS.h, INTERLIS.min, INTERLIS.s, INTERLIS.M, INTERLIS.d
                    fieldType = OFTReal;
                }
                else if (EQUAL(typeName, "IlisMeta07.ModelData.CoordType"))
                {
                    geomType = AddCoord(iliVersion, psName, psElementNode);
                }
                else if (EQUAL(typeName, "IlisMeta07.ModelData.LineType"))
                {
                    const char* psKind = CPLGetXMLValue( psElementNode, "Kind", NULL );
                    if (EQUAL(psKind, "Area"))  // Kind DirectedPolyline, Polyline(CoordType RoadsExdm2ben.Point2D), Area
                    {
                        if (iliVersion == 1)
                        {
                            geomType = AddCoord(iliVersion, psName, psElementNode);
                            //Add geomType = wkbMultiLineString
                        } else {
                            geomType = wkbPolygon;
                        }
                    } else {
                        geomType = wkbMultiLineString;
                    }
                }
                else
                {
                    //MultiValue // e.g. Axes, SurfaceEdge.LineAttrs, SurfaceBoundary.Lines, LineGeometry.Segments
                    //ClassRefType
                    CPLError(CE_Warning, CPLE_NotSupported,
                        "Field '%s' of class %s has unsupported type %s", psName, poTableDefn->GetName(), typeName);
                }
            }
            if (geomType == wkbPolygon && iliVersion == 1)
            {
                psName = "_RefTID";
                fieldType = OFTString;
                geomType = wkbUnknown;
            }
            if (geomType != wkbUnknown)
            {
                OGRGeomFieldDefn fieldDef(psName, geomType);
                //oGFld.SetSpatialRef(geomlayer->GetSpatialRef());
                poTableDefn->AddGeomFieldDefn(&fieldDef);
                CPLDebug( "OGR_ILI", "Adding geometry field %s to Class %s", psName, poTableDefn->GetName());                
            } else {
                OGRFieldDefn fieldDef(psName, fieldType);
                poTableDefn->AddFieldDefn(&fieldDef);
                CPLDebug( "OGR_ILI", "Adding field %s to Class %s", psName, poTableDefn->GetName());                
            }
        }
    }
};


ImdReader::ImdReader(int iliVersionIn) : iliVersion(iliVersionIn) {
}

ImdReader::~ImdReader() {
}

const char* ImdReader::LayerName(const char* psClassTID) {
    if (iliVersion == 1)
    {
        char **papszTokens =
            CSLTokenizeString2( psClassTID, ".", CSLT_ALLOWEMPTYTOKENS );

        static char layername[512]; //FIXME
        layername[0] = '\0';
        for(int i = 1; papszTokens != NULL && papszTokens[i] != NULL; i++)
        {
            if (i>1) strcat(layername, "__");
            strcat(layername, papszTokens[i]);
        }
        CSLDestroy( papszTokens );
        return layername;
    } else {
        return psClassTID;
    }
}

std::list<OGRFeatureDefn*> ImdReader::ReadModel(const char *pszFilename) {
    CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Reading model '%s'", pszFilename);
    std::list<OGRFeatureDefn*> poTableList;

    CPLXMLNode* psRootNode = CPLParseXMLFile(pszFilename);
    if( psRootNode == NULL )
        return poTableList;
    CPLXMLNode *psSectionNode = CPLGetXMLNode( psRootNode, "=TRANSFER.DATASECTION" );
    if( psSectionNode == NULL )
        return poTableList;

    StrNodeMap oTidLookup; /* for fast lookup of REF relations */
    typedef std::map<CPLXMLNode*,IliClass*> ClassesMap; /* all classes with XML node for lookup */
    ClassesMap oClasses;
    const char *modelName;

    /* Fill TID lookup map and IliClasses lookup map */
    CPLXMLNode* psModel = psSectionNode->psChild;
    while( psModel != NULL )
    {
        modelName = CPLGetXMLValue( psModel, "BID", NULL );
        //CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Model: '%s'", modelName);
        CPLXMLNode* psEntry = psModel->psChild;
        while( psEntry != NULL )
        {
            if (psEntry->eType != CXT_Attribute) //ignore BID
            {
                //CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Node tag: '%s'", psEntry->pszValue);
                const char* psTID = CPLGetXMLValue( psEntry, "TID", NULL );
                if( psTID != NULL )
                    oTidLookup[psTID] = psEntry;
                //const char* psName = CPLGetXMLValue( psEntry, "Name", NULL );
                if( EQUAL(psEntry->pszValue, "IlisMeta07.ModelData.Class") && !EQUAL(modelName, "MODEL.INTERLIS"))
                {
                    //CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Class Name: '%s'", psTID);
                    OGRFeatureDefn* poTableDefn = new OGRFeatureDefn(LayerName(psTID));
                    oClasses[psEntry] = new IliClass(poTableDefn);
                }
            }
            psEntry = psEntry->psNext;
        }

        // 2nd pass: add fields via TransferElement entries & role associations
        psEntry = psModel->psChild;
        while( psEntry != NULL )
        {
            if (psEntry->eType != CXT_Attribute) //ignore BID
            {
                //CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Node tag: '%s'", psEntry->pszValue);
                if( EQUAL(psEntry->pszValue, "IlisMeta07.ModelData.TransferElement") && !EQUAL(modelName, "MODEL.INTERLIS"))
                {
                    const char* psClassRef = CPLGetXMLValue( psEntry, "TransferClass.REF", NULL );
                    const char* psElementRef = CPLGetXMLValue( psEntry, "TransferElement.REF", NULL );
                    int iOrderPos = atoi(CPLGetXMLValue( psEntry, "TransferElement.ORDER_POS", "0" ))-1;
                    IliClass* psParentClass = oClasses[oTidLookup[psClassRef]];
                    CPLXMLNode* psElementNode = oTidLookup[psElementRef];
                    psParentClass->AddFieldNode(psElementNode, iOrderPos);
                }
                if( EQUAL(psEntry->pszValue, "IlisMeta07.ModelData.Role") && !EQUAL(modelName, "MODEL.INTERLIS"))
                {
                    const char* psRefParent = CPLGetXMLValue( psEntry, "Association.REF", NULL );
                    int iOrderPos = atoi(CPLGetXMLValue( psEntry, "Association.ORDER_POS", "0" ))-1;
                    IliClass* psParentClass = oClasses[oTidLookup[psRefParent]];
                    if (psParentClass)
                        psParentClass->AddRoleNode(psEntry, iOrderPos);
                }
            }
            psEntry = psEntry->psNext;

        }

        psModel = psModel->psNext;
    }

    /* Analyze class inheritance & add fields to class table defn */
    for (ClassesMap::const_iterator it = oClasses.begin(); it != oClasses.end(); ++it)
    {
        //CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Class: '%s'", it->second->poTableDefn->GetName());
        const char* psRefSuper = CPLGetXMLValue( it->first, "Super.REF", NULL );
        if (psRefSuper)
            oClasses[oTidLookup[psRefSuper]]->hasDerivedClasses = true;
        it->second->AddFieldDefinitions(iliVersion, oTidLookup);
    }

    /* Filter relevant classes */
    for (ClassesMap::const_iterator it = oClasses.begin(); it != oClasses.end(); ++it)
    {
        if (!it->second->hasDerivedClasses && !it->second->isEmbedded())
            poTableList.push_back(it->second->poTableDefn);
    }

    CPLDestroyXMLNode(psRootNode);
    return poTableList;
}
