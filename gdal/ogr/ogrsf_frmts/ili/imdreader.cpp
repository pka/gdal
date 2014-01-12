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
    std::vector<IliClass*> oRoleParents;
    IliClass* psBaseClass;

    IliClass(OGRFeatureDefn* poTableDefnIn) : poTableDefn(poTableDefnIn), oFields(), oRoleParents(), psBaseClass(NULL)
    {
    };
    ~IliClass()
    {
        delete poTableDefn;
    };
    void AddFieldNode(CPLXMLNode* node, int iOrderPos)
    {
        if (iOrderPos >= oFields.size())
            oFields.resize(iOrderPos+1);
        //CPLDebug( "OGR_ILI", "Register field with OrderPos %d to Class %s", iOrderPos, poTableDefn->GetName());
        oFields[iOrderPos] = node;
    }
    void AddRoleNode(CPLXMLNode* node, int iOrderPos, IliClass* psBaseClass)
    {
        AddFieldNode(node, iOrderPos);
        oRoleParents.resize(oFields.size());
        oRoleParents[iOrderPos] = psBaseClass;
    }
    void AddEmbeddedAssocFields()
    {
        CPLXMLNode* embeddedRoleField = NULL;
        for (int i = 0; i < oFields.size(); ++i)
            if (oFields[i])
            {
                if (CSLTestBoolean(CPLGetXMLValue( oFields[i], "EmbeddedTransfer", "FALSE" )))
                {
                    embeddedRoleField = oFields[i];
                }
            }
        if (embeddedRoleField) // append to parent of opposite role
        {
        for (int i = 0; i < oFields.size(); ++i)
            if (oFields[i] != embeddedRoleField)
            {
                IliClass* oRoleParent = oRoleParents[i];
                oRoleParent->AddFieldNode(embeddedRoleField, oRoleParent->oFields.size());
            }
        }
    }
    void AddInhertedFields(IliClass* psDerivedClass)
    {
        if (psBaseClass)
            psBaseClass->AddInhertedFields(psDerivedClass);
        if (psDerivedClass != this)
            for (int i = 0; i < oFields.size(); ++i)
                if (oFields[i])
                    psDerivedClass->AddFieldNode(oFields[i], i);
    }
    void AddFieldDefinitions(StrNodeMap& oTidLookup)
    {
        AddInhertedFields(this);
        // add TID field
        OGRFieldDefn ofieldDefn("TID", OFTString);
        poTableDefn->AddFieldDefn(&ofieldDefn);
        for (NodeVector::const_iterator it = oFields.begin(); it != oFields.end(); ++it)
        {
            if (*it == NULL) continue;
            const char* psName = CPLGetXMLValue( *it, "Name", NULL );
            OGRFieldDefn fieldDef(psName, OFTString); //TODO: determine field type
            poTableDefn->AddFieldDefn(&fieldDef); //TODO: add geometry fields
            CPLDebug( "OGR_ILI", "Adding field %s to Class %s -> %d", fieldDef.GetNameRef(), poTableDefn->GetName(), poTableDefn->GetFieldCount());
        }
    }
};


ImdReader::ImdReader() {
}

ImdReader::~ImdReader() {
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
    typedef std::set<CPLXMLNode*> NodeSet;
    NodeSet oFields; /* AttrOrParam nodes */
    NodeSet oRoles; /* Role nodes */
    std::map<CPLString,const char*> oBaseClasses; /* Assoc REF -> BaseClass REF */
    const char *modelName;

    /* Fill TID lookup map and IliClasses lookup map */
    CPLXMLNode* psModel = psSectionNode->psChild;
    while( psModel != NULL )
    {
        modelName = CPLGetXMLValue( psModel, "BID", NULL );
        CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Model: '%s'", modelName);
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
                    OGRFeatureDefn* poTableDefn = new OGRFeatureDefn(psTID);
                    poTableDefn->SetGeomType( wkbUnknown );
                    // Delete default geometry field
                    //poTableDefn->DeleteGeomFieldDefn(0);
                    oClasses[psEntry] = new IliClass(poTableDefn);
                }
                if( EQUAL(psEntry->pszValue, "IlisMeta07.ModelData.AttrOrParam") && !EQUAL(modelName, "MODEL.INTERLIS"))
                {
                    //CPLDebug( "ImdReader::ReadModel   OGR_ILI", "AttrOrParam Name: '%s'", psName);
                    oFields.insert(psEntry);
                }
                if( EQUAL(psEntry->pszValue, "IlisMeta07.ModelData.Role") && !EQUAL(modelName, "MODEL.INTERLIS"))
                {
                    //CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Role Name: '%s'", psName);
                    oRoles.insert(psEntry);
                }
                if( EQUAL(psEntry->pszValue, "IlisMeta07.ModelData.BaseClass") && !EQUAL(modelName, "MODEL.INTERLIS"))
                {
                    const char* psEntryRef = CPLGetXMLValue( psEntry, "CRT.REF", NULL );
                    const char* psBaseClassRef = CPLGetXMLValue( psEntry, "BaseClass.REF", NULL );
                    //CPLDebug( "ImdReader::ReadModel   OGR_ILI", "BaseClass: '%s'->'%s'", psEntryRef, psBaseClassRef);
                    oBaseClasses[psEntryRef] = psBaseClassRef;
                }
            }
            psEntry = psEntry->psNext;
        }

        psModel = psModel->psNext;
    }

    /* Collect fields */
    for (NodeSet::const_iterator it = oFields.begin(); it != oFields.end(); ++it)
    {
        const char* psName = CPLGetXMLValue( *it, "Name", NULL );
        const char* psRefParent = CPLGetXMLValue( *it, "AttrParent.REF", NULL );
        int iOrderPos = atoi(CPLGetXMLValue( *it, "AttrParent.ORDER_POS", "0" ))-1;
        CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Field: '%s' [%d] (%s)", psName, iOrderPos, psRefParent);
        IliClass* psParentClass = oClasses[oTidLookup[psRefParent]];
        if (psParentClass)
        {
            psParentClass->AddFieldNode(*it, iOrderPos);
        } else {
            CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Error in AttrParent lookup of Field: '%s'", psName);
        }
    }
    for (NodeSet::const_iterator it = oRoles.begin(); it != oRoles.end(); ++it)
    {
        const char* psName = CPLGetXMLValue( *it, "Name", NULL );
        const char* psRefParent = CPLGetXMLValue( *it, "Association.REF", NULL );
        int iOrderPos = atoi(CPLGetXMLValue( *it, "Association.ORDER_POS", "0" ))-1;
        CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Role: '%s' [%d] (%s)", psName, iOrderPos, psRefParent);
        IliClass* psParentClass = oClasses[oTidLookup[psRefParent]];
        const char* psTID = CPLGetXMLValue( *it, "TID", NULL );
        IliClass* psBaseClass = oClasses[oTidLookup[oBaseClasses[psTID]]];
        if (psParentClass && psBaseClass)
        {
            psParentClass->AddRoleNode(*it, iOrderPos, psBaseClass);
        } else {
            CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Error in Association lookup of Role: '%s'", psName);
        }
    }

    /* Analyze class inheritance */
    for (ClassesMap::const_iterator it = oClasses.begin(); it != oClasses.end(); ++it)
    {
        //CPLDebug( "ImdReader::ReadModel   OGR_ILI", "Class: '%s'", it->second->poTableDefn->GetName());
        const char* psRefSuper = CPLGetXMLValue( it->first, "Super.REF", NULL );
        if (psRefSuper)
            it->second->psBaseClass = oClasses[oTidLookup[psRefSuper]];
        it->second->AddEmbeddedAssocFields();
    }

    /* Add fields to class table defn */
    for (ClassesMap::const_iterator it = oClasses.begin(); it != oClasses.end(); ++it)
    {
        it->second->AddFieldDefinitions(oTidLookup);
        poTableList.push_back(it->second->poTableDefn); //TODO: omit classes with descendants
    }

    CPLDestroyXMLNode(psRootNode);
    return poTableList;
}
