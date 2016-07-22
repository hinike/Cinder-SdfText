/*
Copyright 2016 Google Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
Copyright (c) 2016, The Cinder Project, All rights reserved.
This code is intended for use with the Cinder C++ library: http://libcinder.org
Redistribution and use in source and binary forms, with or without modification, are permitted provided that
the following conditions are met:
* Redistributions of source code must retain the above copyright notice, this list of conditions and
the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
the following disclaimer in the documentation and/or other materials provided with the distribution.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#include "cinder/gl/SdfText.h"
#include "cinder/gl/Context.h"
#include "cinder/gl/Shader.h"
#include "cinder/gl/Vao.h"
#include "cinder/gl/Vbo.h"
#include "cinder/gl/scoped.h"
#include "cinder/ip/Fill.h"
#include "cinder/ImageIo.h"
#include "cinder/Log.h"
#include "cinder/Text.h"
#include "cinder/Unicode.h"
#include "cinder/Utilities.h"

#include "cinder/app/App.h"

#include "ft2build.h"
#include FT_FREETYPE_H
#include "freetype/ftsnames.h"
#include "freetype/ttnameid.h"

#include "msdfgen/msdfgen.h"
#include "msdfgen/util.h"

#include <cmath>
#include <set>
#include <vector>
#include <boost/algorithm/string.hpp>

#if defined( CINDER_MSW )
	#include <Windows.h>
#endif

static const float MAX_SIZE = 1000000.0f;

namespace cinder { namespace gl {

static std::string kSdfVertShader = 
	"#version 150\n"
	"uniform mat4 ciModelViewProjection;\n"
	"in vec4 ciPosition;\n"
	"in vec2 ciTexCoord0;\n"
	"out vec2 TexCoord;\n"
	"void main()\n"
	"{\n"
	"	gl_Position = ciModelViewProjection * ciPosition;\n"
	"	TexCoord = ciTexCoord0;\n"
	"}\n";

static std::string kSdfFragShader = 
	"#version 150\n"
	"uniform sampler2D uTex0;\n"
	"uniform vec4      uBgColor;\n"
	"uniform vec4      uFgColor;\n"
	"in vec2           TexCoord;\n"
	"out vec4          Color;\n"
	"\n"
	"float median( float r, float g, float b ) {\n"
	"	return max( min( r, g ), min( max( r, g ), b ) );\n"
	"}\n"
	"\n"
	"void main() {\n"
	"	vec3 sample = texture( uTex0, TexCoord ).rgb;\n"
	"	float sigDist = median( sample.r, sample.g, sample.b ) - 0.5;\n"
	"	float opacity = clamp( sigDist / fwidth(sigDist) + 0.5, 0.0, 1.0 );\n"
	"	Color = mix( uBgColor, uFgColor, opacity );\n"
	//"	Color = vec4( 1, 0, 0, 1 );\n"
	"}\n";

static gl::GlslProgRef sDefaultShader;

// =================================================================================================
// SdfText::TextureAtlas
// =================================================================================================
class SdfText::TextureAtlas {
public:

	struct GlyphInfo {
		uint32_t	mTextureIndex;
		Area		mTexCoords;
		vec2		mOriginOffset;
	};

	// ---------------------------------------------------------------------------------------------

	using CharMap = std::unordered_map<uint32_t, SdfText::Font::Glyph>;
	using GlyphMap = std::unordered_map<SdfText::Font::Glyph, GlyphInfo>;

	// ---------------------------------------------------------------------------------------------

	struct CacheKey {
		std::string mFamilyName;
		std::string mStyleName;
		std::string mUtf8Chars;
		ivec2		mTextureSize = ivec2( 0 );
		ivec2		mSdfBitmapSize = ivec2( 0 );
		bool operator==( const CacheKey& rhs ) const { 
			return ( mFamilyName == rhs.mFamilyName ) &&
				   ( mStyleName == rhs.mStyleName ) && 
				   ( mUtf8Chars == rhs.mUtf8Chars ) &&
				   ( mTextureSize == rhs.mTextureSize ) &&
				   ( mSdfBitmapSize == rhs.mSdfBitmapSize );
		}
		bool operator!=( const CacheKey& rhs ) const {
			return ( mFamilyName != rhs.mFamilyName ) ||
				   ( mStyleName != rhs.mStyleName ) || 
				   ( mUtf8Chars != rhs.mUtf8Chars ) ||
				   ( mTextureSize != rhs.mTextureSize ) ||
				   ( mSdfBitmapSize != rhs.mSdfBitmapSize );
		}
	};

	typedef std::vector<std::pair<CacheKey, SdfText::TextureAtlasRef>> AtlasCacher;

	// ---------------------------------------------------------------------------------------------

	virtual ~TextureAtlas() {}

	static SdfText::TextureAtlasRef create( FT_Face face, const SdfText::Format &format, const std::string &utf8Chars );

	static ivec2 calculateSdfBitmapSize( const vec2 &sdfScale, const ivec2& sdfPadding, const vec2 &maxGlyphSize );

private:
	TextureAtlas( FT_Face face, const SdfText::Format &format, const std::string &utf8Chars );
	friend class SdfText;

	FT_Face						mFace = nullptr;
	std::vector<gl::TextureRef>	mTextures;
	CharMap						mCharMap;
	GlyphMap					mGlyphMap;

	//! Base scale that SDF generator uses is size 32 at 72 DPI. A scale of 1.5, 2.0, and 3.0 translates to size 48, 64 and 96 and 72 DPI.
	vec2						mSdfScale = vec2( 1.0f );
	vec2						mSdfPadding = vec2( 2.0f );
	ivec2						mSdfBitmapSize = ivec2( 0 );
	vec2						mMaxGlyphSize = vec2( 0.0f );
	float						mMaxAscent = 0.0f;
	float						mMaxDescent = 0.0f;

};

SdfText::TextureAtlas::TextureAtlas( FT_Face face, const SdfText::Format &format, const std::string &utf8Chars )
	: mFace( face ), mSdfScale( format.getSdfScale() ), mSdfPadding( format.getSdfPadding() )
{
	std::u32string utf32Chars = ci::toUtf32( utf8Chars );
	// Add a space if needed
	if( std::string::npos == utf8Chars.find( ' ' ) ) {
		utf32Chars += ci::toUtf32( " " );
	}

	// Build the maps and information pieces that will be needed later
	std::set<SdfText::Font::Glyph> glyphIndices;
	for( const auto& ch : utf32Chars ) {
		FT_UInt glyphIndex = FT_Get_Char_Index( face, static_cast<FT_ULong>( ch ) );
		glyphIndices.insert( glyphIndex );

		// Character to glyph index
		mCharMap[static_cast<uint32_t>( ch )] = glyphIndex;

		// Glyph bounds, 
		msdfgen::Shape shape;
		if( msdfgen::loadGlyph( shape, face, glyphIndex ) ) {
			double l, b, r, t;
			l = b = r = t = 0.0;
			shape.bounds( l, b, r, t );
			// Glyph bounds
			Rectf bounds = Rectf( l, b, r, t );
			mGlyphMap[glyphIndex].mOriginOffset = vec2( l, b );
			// Max glyph size
			mMaxGlyphSize.x = std::max( mMaxGlyphSize.x, bounds.getWidth() );
			mMaxGlyphSize.y = std::max( mMaxGlyphSize.y, bounds.getHeight() );
			// Max ascent, descent
			mMaxAscent = std::max( mMaxAscent, static_cast<float>( t ) );
			mMaxDescent = std::max( mMaxAscent, static_cast<float>( std::fabs( b ) ) );
			//CI_LOG_I( (char)ch << " : " << bounds );
		}	
	}

	// Determine render bitmap size
	mSdfBitmapSize = SdfText::TextureAtlas::calculateSdfBitmapSize( mSdfScale, mSdfPadding, mMaxGlyphSize );
	// Determine glyph counts (per texture atlas)
	const size_t numGlyphColumns   = ( format.getTextureWidth()  / mSdfBitmapSize.x ) - 1;
	const size_t numGlyphRows      = ( format.getTextureHeight() / mSdfBitmapSize.y ) - 1;
	const size_t numGlyphsPerAtlas = numGlyphColumns * numGlyphRows;
	
	// Render position for each glyph
	struct RenderGlyph {
		uint32_t glyphIndex;
		ivec2    position;
	};

	std::vector<std::vector<RenderGlyph>> renderAtlases;

	// Build the atlases
	size_t curRenderIndex = 0;
	ivec2 curRenderPos = ivec2( 0 );
	std::vector<RenderGlyph> curRenderGlyphs;
	for( std::set<SdfText::Font::Glyph>::const_iterator glyphIndexIt = glyphIndices.begin(); glyphIndexIt != glyphIndices.end() ;  ) {
		// Build render glyph
		RenderGlyph renderGlyph;
		renderGlyph.glyphIndex = *glyphIndexIt;
		renderGlyph.position.x = curRenderPos.x;
		renderGlyph.position.y = curRenderPos.y;
		
		// Add to render atlas
		curRenderGlyphs.push_back( renderGlyph );

		// Increment index
		++curRenderIndex;
		// Increment glyph index iterator
		++glyphIndexIt;
		// Advance horizontal position
		curRenderPos.x += mSdfBitmapSize.x;
		curRenderPos.x += 1;
		// Move to next row if needed
		if( 0 == ( curRenderIndex % numGlyphColumns ) ) {
			curRenderPos.x = 0.0f;
			curRenderPos.y += mSdfBitmapSize.y;
			curRenderPos.y += 1;
		}

		if( ( numGlyphsPerAtlas == curRenderIndex ) || ( glyphIndices.end() == glyphIndexIt ) ) {
			// Copy current atlas
			renderAtlases.push_back( curRenderGlyphs );
			// Reset values
			curRenderIndex = 0;
			curRenderPos = ivec2( 0 );
			curRenderGlyphs.clear();
		}
	}

	// Surface
	Surface8u surface( format.getTextureWidth(), format.getTextureHeight(), false );
	ip::fill( &surface, Color8u( 0, 0, 0 ) );
	uint8_t *surfaceData   = surface.getData();
	size_t surfacePixelInc = surface.getPixelInc();
	size_t surfaceRowBytes = surface.getRowBytes();

	// Render the atlases
	msdfgen::Bitmap<msdfgen::FloatRGB> sdfBitmap( mSdfBitmapSize.x, mSdfBitmapSize.y );
	uint32_t currentTextureIndex = 0;
	for( size_t atlasIndex = 0; atlasIndex < renderAtlases.size(); ++atlasIndex ) {
		const auto& renderGlyphs = renderAtlases[atlasIndex];
		// Render atlas
		for( const auto& renderGlyph : renderGlyphs ) {
			msdfgen::Shape shape;
			if( msdfgen::loadGlyph( shape, face, renderGlyph.glyphIndex ) ) {
				shape.inverseYAxis = true;
				shape.normalize();	
				
				// Edge color
				msdfgen::edgeColoringSimple( shape, 3.0 );
					
				// Generate SDF
				vec2 originOffset = mGlyphMap[renderGlyph.glyphIndex].mOriginOffset;
				float tx = 0.0f + mSdfPadding.x;
				float ty = std::fabs( originOffset.y ) + mSdfPadding.y;
				msdfgen::generateMSDF( sdfBitmap, shape, 4.0, msdfgen::Vector2( mSdfScale.x, mSdfScale.y ), msdfgen::Vector2( tx, ty ) );

				// Copy bitmap
				size_t dstOffset = ( renderGlyph.position.y * surfaceRowBytes ) + ( renderGlyph.position.x * surfacePixelInc );
				uint8_t *dst = surfaceData + dstOffset;
				for( int n = 0; n < mSdfBitmapSize.y; ++n ) {
					Color8u *dstPixel = reinterpret_cast<Color8u *>( dst );
					for( int m = 0; m < mSdfBitmapSize.x; ++m ) {
						msdfgen::FloatRGB &src = sdfBitmap( m, n );
						Color srcPixel = Color( src.r, src.g, src.b );
						*dstPixel = srcPixel;
						++dstPixel;
					}
					dst += surfaceRowBytes;
				}

				// Tex coords
				mGlyphMap[renderGlyph.glyphIndex].mTextureIndex = currentTextureIndex;
				mGlyphMap[renderGlyph.glyphIndex].mTexCoords = Area( 0, 0, mSdfBitmapSize.x, mSdfBitmapSize.y ) + renderGlyph.position;
			}
		}
		// Create texture
		gl::TextureRef tex = gl::Texture::create( surface );
		mTextures.push_back( tex );
		++currentTextureIndex;

		// Debug output
		// DEBUG: writeImage( "sdfText_" + std::to_string( atlasIndex ) + ".png", surface );

		// Reset
		ip::fill( &surface, Color8u( 0, 0, 0 ) );		
	}
}

SdfText::TextureAtlasRef SdfText::TextureAtlas::create( FT_Face face, const SdfText::Format &format, const std::string &utf8Chars )
{
	SdfText::TextureAtlasRef result = SdfText::TextureAtlasRef( new SdfText::TextureAtlas( face, format, utf8Chars ) );
	return result;
}

cinder::ivec2 SdfText::TextureAtlas::calculateSdfBitmapSize( const vec2 &sdfScale, const ivec2& sdfPadding, const vec2 &maxGlyphSize )
{
	ivec2 result = ivec2( ( sdfScale * ( maxGlyphSize + ( 2.0f * vec2( sdfPadding ) ) ) ) + vec2( 0.5f ) );
	return result;
}

// =================================================================================================
// SdfTextManager
// =================================================================================================
class SdfTextManager {
public:
	~SdfTextManager();

	static SdfTextManager			*instance();

	FT_Library						getLibrary() const { return mLibrary; }

	const std::vector<std::string>&	getNames( bool forceRefresh );
	SdfText::Font					getDefault() const;

	struct FontInfo {
		std::string 	key;
		std::string 	name;
		fs::path 		path;
		FontInfo() {}
		FontInfo( const std::string& aKey, const std::string& aName, const fs::path& aPath ) 
			: key( aKey ), name( aName ), path( aPath ) {}
	};

	FontInfo 						getFontInfo( const std::string& fontName ) const;

private:
	SdfTextManager();

	static SdfTextManager			*sInstance;

	FT_Library						mLibrary = nullptr;
	bool							mFontsEnumerated = false;
	std::vector<std::string>		mFontNames;
	std::vector<FontInfo>			mFontInfos;
	std::set<FT_Face>				mTrackedFaces;
	mutable SdfText::Font			mDefault;

	SdfText::TextureAtlas::AtlasCacher		mTrackedTextureAtlases;

	void							acquireFontNamesAndPaths();
	void							faceCreated( FT_Face face );
	void							faceDestroyed( FT_Face face );

	SdfText::TextureAtlasRef		getTextureAtlas( FT_Face face, const SdfText::Format &format, const std::string &utf8Chars );

	friend class SdfText;
	friend class SdfText::FontData;
	friend bool SdfTextFontManager_destroyStaticInstance();
};

// =================================================================================================
// SdfTextBox
// =================================================================================================
class SdfTextBox {
public:
	typedef enum Alignment { LEFT, CENTER, RIGHT } Alignment;
	enum { GROW = 0 };
	
	SdfTextBox() : mAlign( LEFT ), mSize( GROW, GROW ), mFont( SdfText::Font::getDefault() ), mInvalid( true ), mLigate( true ) {}

	SdfTextBox&				size( ivec2 sz ) { setSize( sz ); return *this; }
	SdfTextBox&				size( int width, int height ) { setSize( ivec2( width, height ) ); return *this; }
	ivec2					getSize() const { return mSize; }
	void					setSize( ivec2 sz ) { mSize = sz; mInvalid = true; }

	SdfTextBox&				text( const std::string &t ) { setText( t ); return *this; }
	const std::string&		getText() const { return mText; }
	void					setText( const std::string &t ) { mText = t; mInvalid = true; }
	void					appendText( const std::string &t ) { mText += t; mInvalid = true; }

	SdfTextBox&				font( const SdfText::Font &f ) { setFont( f ); return *this; }
	const SdfText::Font&	getFont() const { return mFont; }
	void					setFont( const SdfText::Font &f ) { mFont = f; mInvalid = true; }

	SdfTextBox&				ligate( bool ligateText = true ) { setLigate( ligateText ); return *this; }
	bool					getLigate() const { return mLigate; }
	void					setLigate( bool ligateText ) { mLigate = ligateText; }

	std::vector<std::string>							calculateLineBreaks( const SdfText::Font::GlyphMetricsMap &cachedGlyphMetrics ) const;
	std::vector<std::pair<SdfText::Font::Glyph,vec2>>	measureGlyphs( const SdfText::Font::GlyphMetricsMap &cachedGlyphMetrics, const SdfText::DrawOptions& drawOptions ) const;

private:
	Alignment		mAlign;
	ivec2			mSize;
	std::string		mText;
	SdfText::Font	mFont;
	bool			mLigate;
	mutable bool	mInvalid;
};

// =================================================================================================
// SdfTexttManager Implementation
// =================================================================================================
SdfTextManager* SdfTextManager::sInstance = nullptr;

bool SdfTextFontManager_destroyStaticInstance() 
{
	if( nullptr != SdfTextManager::sInstance ) {
		delete SdfTextManager::sInstance;
		SdfTextManager::sInstance = nullptr;
	}
	return true;
}

SdfTextManager::SdfTextManager()
{
	FT_Error ftRes = FT_Init_FreeType( &mLibrary );
	if( FT_Err_Ok != ftRes ) {
		throw FontInvalidNameExc("Failed to initialize FreeType2");
	}

	acquireFontNamesAndPaths();
#if defined( CINDER_MSW )
	// Registry operations can be rejected by Windows so no fonts will be picked up 
	// on the initial scan. So we can multiple times.
	if( mFontInfos.empty() ) {
		for( int i = 0; i < 5; ++i ) {
			acquireFontNamesAndPaths();
			if( ! mFontInfos.empty() ) {
				break;
			}
			::Sleep( 10 );
		}
	}
#endif
}

SdfTextManager::~SdfTextManager()
{
	if( nullptr != mLibrary ) {
		for( auto& face : mTrackedFaces ) {
			FT_Done_Face( face );
		}

		FT_Done_FreeType( mLibrary );
	}

#if defined( CINDER_MAC )
	[nsFontManager release];
#elif defined( CINDER_WINRT )
#elif defined( CINDER_ANDROID ) || defined( CINDER_LINUX )
#endif
}

SdfTextManager* SdfTextManager::instance()
{
	if( nullptr == SdfTextManager::sInstance ) {
		SdfTextManager::sInstance =  new SdfTextManager();
		if( nullptr != SdfTextManager::sInstance ) {
			ci::app::App::get()->getSignalShouldQuit().connect( SdfTextFontManager_destroyStaticInstance );
		}
	}
	
	return SdfTextManager::sInstance;
}

#if defined( CINDER_MAC )
void SdfTextManager::acquireFontNamesAndPaths()
{
}
#elif defined( CINDER_MSW )
void SdfTextManager::acquireFontNamesAndPaths()
{
	static const LPWSTR kFontRegistryPath = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";

	// Open Windows font registry key
	HKEY hKey = nullptr;
	LONG result = RegOpenKeyEx( HKEY_LOCAL_MACHINE, kFontRegistryPath, 0, KEY_READ, &hKey );
	if( ERROR_SUCCESS != result ) {
		return;
	}
	
	// Get info for registry key
	DWORD maxValueNameSize, maxValueDataSize;
	result = RegQueryInfoKey( hKey, 0, 0, 0, 0, 0, 0, 0, &maxValueNameSize, &maxValueDataSize, 0, 0 );
	if( ERROR_SUCCESS != result ) {
		return;
	}

	DWORD valueIndex = 0;
	LPWSTR valueName = new WCHAR[maxValueNameSize];
	LPBYTE valueData = new BYTE[maxValueDataSize];
	DWORD valueNameSize, valueDataSize, valueType;

	// Enumerate registry keys
	do {
		valueDataSize = maxValueDataSize;
		valueNameSize = maxValueNameSize;

		// Read registry key values
		result = RegEnumValue( hKey, valueIndex, valueName, &valueNameSize, 0, &valueType, valueData, &valueDataSize );
		++valueIndex;

		// Bail if read fails
		if( ( ERROR_SUCCESS != result ) || ( REG_SZ != valueType ) ) {
			continue;
		}

		// Build font info
		const std::string kTrueTypeTag = "(TrueType)";
		std::wstring wsFontName = std::wstring( valueName, valueNameSize );
		std::wstring wsFontFilePath = std::wstring( reinterpret_cast<LPWSTR>( valueData ), valueDataSize );
		std::string fontName = ci::toUtf8( reinterpret_cast<const char16_t *>( wsFontName.c_str() ), wsFontName.length() * sizeof( char16_t ) );
		std::string fontFilePath = ci::toUtf8( reinterpret_cast<const char16_t *>( wsFontFilePath.c_str() ), wsFontFilePath.length() * sizeof( char16_t ) );
		if( std::string::npos != fontName.find( kTrueTypeTag ) ) {
			boost::replace_all( fontName, kTrueTypeTag, "" );
			std::string fontKey = boost::to_lower_copy( fontName );
			auto it = std::find_if( std::begin( mFontInfos ), std::end( mFontInfos ),
				[fontKey]( const FontInfo& elem ) -> bool {
					return elem.key == fontKey;
				}
			);
			if( std::end( mFontInfos ) == it ) {
				FontInfo fontInfo = FontInfo( fontKey, fontName, "C:\\Windows\\Fonts\\" + fontFilePath );
				mFontInfos.push_back( fontInfo );
				mFontNames.push_back( fontName );
			}
		}
	} 
	while( ERROR_NO_MORE_ITEMS != result  );

	delete [] valueName;
	delete [] valueData;
}
#elif defined( CINDER_WINRT ) 
void SdfTextManager::acquireFontNamesAndPaths()
{
}
#elif defined( CINDER_ANDROID )
void SdfTextManager::acquireFontNamesAndPaths()
{
	fs::path systemFontDir = "/system/fonts";
	if( fs::exists( systemFontDir ) && fs::is_directory( systemFontDir ) ) {
		fs::directory_iterator end_iter;
		for( fs::directory_iterator dir_iter( systemFontDir ) ; dir_iter != end_iter ; ++dir_iter ) {
			if( fs::is_regular_file( dir_iter->status() ) ) {
				fs::path fontPath = dir_iter->path();

				FT_Face tmpFace;
				FT_Error error = FT_New_Face( mLibrary, fontPath.string().c_str(), 0, &tmpFace );
				if( error ) {
					continue;
				}

				std::string fontName = ci::linux::ftutil::GetFontName( tmpFace, fontPath.stem().string() );
				std::string keyName = fontName;
				std::transform( keyName.begin(), keyName.end(), keyName.begin(), [](char c) -> char { return (c >= 'A' && c <='Z') ? (c + 32) : c; } );
				mFontInfos.push_back( FontInfo( keyName, fontName, fontPath ) );

				const std::string regular = "regular";
				size_t startPos = keyName.find( regular );
				if( std::string::npos != startPos ) {
					keyName.replace( startPos, regular.length(), "" );
					mFontInfos.push_back( FontInfo( keyName, fontName, fontPath ) );
				} 	

				FT_Done_Face( tmpFace );
			}
		}
	}
}
#elif defined( CINDER_LINUX )
void SdfTextManager::acquireFontNamesAndPaths()
{
}
#endif

void SdfTextManager::faceCreated( FT_Face face ) 
{
	mTrackedFaces.insert( face );
}

void SdfTextManager::faceDestroyed( FT_Face face ) 
{
	mTrackedFaces.erase( face );
}

SdfText::TextureAtlasRef SdfTextManager::getTextureAtlas( FT_Face face, const SdfText::Format &format, const std::string &utf8Chars )
{
	std::u32string utf32Chars = ci::toUtf32( utf8Chars );
	// Add a space if needed
	if( std::string::npos == utf8Chars.find( ' ' ) ) {
		utf32Chars += ci::toUtf32( " " );
	}

	// Build the maps and information pieces that will be needed later
	vec2 maxGlyphSize = vec2( 0 );
	for( const auto& ch : utf32Chars ) {
		FT_UInt glyphIndex = FT_Get_Char_Index( face, static_cast<FT_ULong>( ch ) );
		// Glyph bounds, 
		msdfgen::Shape shape;
		if( msdfgen::loadGlyph( shape, face, glyphIndex ) ) {
			double l, b, r, t;
			l = b = r = t = 0.0;
			shape.bounds( l, b, r, t );
			// Glyph bounds
			Rectf bounds = Rectf( l, b, r, t );
			// Max glyph size
			maxGlyphSize.x = std::max( maxGlyphSize.x, bounds.getWidth() );
			maxGlyphSize.y = std::max( maxGlyphSize.y, bounds.getHeight() );
		}	
	}
	
	SdfText::TextureAtlas::CacheKey key;
	key.mFamilyName = std::string( face->family_name );
	key.mStyleName = std::string( face->style_name );
	key.mUtf8Chars = utf8Chars;
	key.mTextureSize = format.getTextureSize();
	key.mSdfBitmapSize = SdfText::TextureAtlas::calculateSdfBitmapSize( format.getSdfScale(), format.getSdfPadding(), maxGlyphSize );

	// Result
	SdfText::TextureAtlasRef result;
	// Look for the texture atlas 
	auto it = std::find_if( std::begin( mTrackedTextureAtlases ), std::end( mTrackedTextureAtlases ),
		[key]( const std::pair<SdfText::TextureAtlas::CacheKey, SdfText::TextureAtlasRef>& elem ) -> bool {
			return elem.first == key;
		}
	);
	// Use the texture atlas if a matching one is found
	if( mTrackedTextureAtlases.end() != it ) {
		result = it->second;
	}
	// ...otherwise build a new one
	else {
		result = SdfText::TextureAtlas::create( face, format, utf8Chars );
	}

	return result;
}

SdfTextManager::FontInfo SdfTextManager::getFontInfo( const std::string& fontName ) const
{
	SdfTextManager::FontInfo result;

#if defined( CINDER_MAC )
#elif defined( CINDER_MSW ) || defined( CINDER_WINRT )
	result.key  = "arial";
	result.name = "Arial";
	result.path = "C:\\Windows\\Fonts\\arial.ttf";
#elif defined( CINDER_ANDROID )
	result.key  = "roboto regular";
	result.name = "Roboto Regular";
	result.path = "/system/fonts/Roboto-Regular.ttf";
#elif defined( CINDER_LINUX )	
#endif

	std::string lcfn = boost::to_lower_copy( fontName );
	std::vector<std::string> tokens = ci::split( lcfn, ' ' );
	float highScore = 0.0f;
	for( const auto& fontInfos : mFontInfos ) {
		int hits = 0;
		for( const auto& tok : tokens ) {
			if( std::string::npos != fontInfos.key.find( tok ) ) {
				hits += tok.size();	
			}
		}

		if( hits > 0 ) {
			float score = (float)hits/(float)(fontInfos.key.length());
			if( score > highScore ) {
				highScore = score;
				result = fontInfos;
			}
		}
	}

	return result;
}

const std::vector<std::string>& SdfTextManager::getNames( bool forceRefresh )
{
	if( ( ! mFontsEnumerated ) || forceRefresh ) {
		mFontInfos.clear();
		mFontNames.clear();

		acquireFontNamesAndPaths();
		// Registry operations can be rejected by Windows so no fonts will be picked up 
		// on the initial scan. So we can multiple times.
		if( mFontInfos.empty() ) {
			for( int i = 0; i < 5; ++i ) {
				acquireFontNamesAndPaths();
				if( ! mFontInfos.empty() ) {
					break;
				}
				::Sleep( 10 );
			}
		}

		mFontsEnumerated = true;
	}

/*
	if( ( ! mFontsEnumerated ) || forceRefresh ) {
		mFontNames.clear();
#if defined( CINDER_MAC )
		NSArray *fontNames = [nsFontManager availableFonts];
		for( NSString *fontName in fontNames ) {
			mFontNames.push_back( string( [fontName UTF8String] ) );
		}
#elif defined( CINDER_COCOA_TOUCH )
		NSArray *familyNames = [UIFont familyNames];
		for( NSString *familyName in familyNames ) {
			NSArray *fontNames = [UIFont fontNamesForFamilyName:familyName];
			for( NSString *fontName in fontNames ) {
				mFontNames.push_back( string( [fontName UTF8String] ) );
			}
		}
#elif defined( CINDER_MSW )
	acquireFontPaths();
	// Registry operations can be rejected by Windows so no fonts will be picked up 
	// on the initial scan. So we can multiple times.
	if( mFontInfos.empty() ) {
		for( int i = 0; i < 5; ++i ) {
			acquireFontPaths();
			if( ! mFontInfos.empty() ) {
				break;
			}
			::Sleep( 10 );
		}
	}
#elif defined( CINDER_WINRT )
		Platform::Array<Platform::String^>^ fontNames = FontEnumeration::FontEnumerator().ListSystemFonts();
		for( unsigned i = 0; i < fontNames->Length; ++i ) {
			const wchar_t *start = fontNames[i]->Begin();
			const wchar_t *end = fontNames[i]->End();
			mFontNames.push_back(std::string(start, end));
		}
#elif defined( CINDER_ANDROID )
		std::set<std::string> uniqueNames;
		for( const auto& fontInfos : mFontInfos ) {
			uniqueNames.insert( fontInfos.name );
		}

		for( const auto& name : uniqueNames ) {
			mFontNames.push_back( name );
		}
#elif defined( CINDER_LINUX )
		if( ::FcInit() ) {
			::FcPattern   *pat = ::FcPatternCreate();
			::FcObjectSet *os  = ::FcObjectSetBuild( FC_FILE, FC_FAMILY, FC_STYLE, (char *)0 );
			::FcFontSet   *fs  = ::FcFontList (0, pat, os);
		
			for( size_t i = 0; i < fs->nfont; ++i ) {
				::FcPattern *font = fs->fonts[i];
				::FcChar8 *family = nullptr;
				if( ::FcPatternGetString( font, FC_FAMILY, 0, &family ) == FcResultMatch ) 
				{					
					string fontName = std::string( (const char*)family );
					mFontNames.push_back( fontName );
				}
			}

			::FcObjectSetDestroy( os );
			::FcPatternDestroy( pat );
			::FcFontSetDestroy( fs );

			::FcFini();
		}
#endif
		mFontsEnumerated = true;
	}
*/	

	return mFontNames;
}

SdfText::Font SdfTextManager::getDefault() const
{
	if( ! mDefault ) {
#if defined( CINDER_COCOA )        
		mDefault = SdfText::Font( "Helvetica", 32.0f );
#elif defined( CINDER_MSW ) || defined( CINDER_WINRT )
		mDefault = SdfText::Font( "Arial", 32.0f );
#elif defined( CINDER_ANDROID ) || defined( CINDER_LINUX )
		mDefault = SdfText::Font( "Roboto", 32.0f );
#endif
	}

	return mDefault;
}

// =================================================================================================
// SdfTextBox Implementation
// =================================================================================================
struct LineProcessor 
{
	LineProcessor( std::vector<std::string> *strings ) : mStrings( strings ) {}
	void operator()( const char *line, size_t len ) const { mStrings->push_back( std::string( line, len ) ); }
	mutable std::vector<std::string> *mStrings = nullptr;
};

struct LineMeasure 
{
	LineMeasure( int maxWidth, const SdfText::Font &font, const SdfText::Font::GlyphMetricsMap &cachedGlyphMetrics ) 
		: mMaxWidth( maxWidth ), mFace( font.getFace() ), mCachedGlyphMerics( cachedGlyphMetrics ) {}

	bool operator()( const char *line, size_t len ) const {
		if( mMaxWidth >= MAX_SIZE ) {
			// too big anyway so just return true
			return true;
		}

		std::u32string utf32Chars = ci::toUtf32( std::string( line, len ) );
		int measuredWidth = 0;
		FT_Vector pen = { 0, 0 };
		for( const auto& ch : utf32Chars ) {
			ivec2 advance = { 0, 0 };
			FT_UInt glyphIndex = FT_Get_Char_Index( mFace, ch );

			auto iter = mCachedGlyphMerics.find( glyphIndex );
			advance = iter->second.advance;		

			pen.x += advance.x;
			pen.y += advance.y;

			measuredWidth = ( pen.x >> 6 );
		}

		bool result = ( measuredWidth <= mMaxWidth );
		return result;
	}

	int										mMaxWidth = 0;
	FT_Face									mFace = nullptr;
	const SdfText::Font::GlyphMetricsMap	&mCachedGlyphMerics;
};

std::vector<std::string> SdfTextBox::calculateLineBreaks( const SdfText::Font::GlyphMetricsMap &cachedGlyphMetrics ) const
{
	std::vector<std::string> result;
	std::function<void(const char *,size_t)> lineFn = LineProcessor( &result );		
	lineBreakUtf8( mText.c_str(), LineMeasure( ( mSize.x > 0 ) ? mSize.x : MAX_SIZE, mFont, cachedGlyphMetrics ), lineFn );
	return result;
}

SdfText::Font::GlyphMeasures SdfTextBox::measureGlyphs( const SdfText::Font::GlyphMetricsMap &cachedGlyphMetrics, const SdfText::DrawOptions& drawOptions ) const
{
	SdfText::Font::GlyphMeasures result;

	if( mText.empty() ) {
		return result;
	}

	FT_Face face = mFont.getFace();
	std::vector<std::string> mLines = calculateLineBreaks( cachedGlyphMetrics );

	const float fontSizeScale = mFont.getSize() / 32.0f;
	const float ascent        = mFont.getAscent();
	const float descent       = mFont.getDescent();
	const float leading       = mFont.getLeading() + drawOptions.getLeading();
	const float lineHeight    = fontSizeScale * ( ascent + descent + leading );

	float curY = 0;
	for( std::vector<std::string>::const_iterator lineIt = mLines.begin(); lineIt != mLines.end(); ++lineIt ) {
		std::u32string utf32Chars = ci::toUtf32( *lineIt );

		FT_Vector pen = { 0, 0 };
		for( const auto& ch : utf32Chars ) {
			ivec2 advance = { 0, 0 };
			FT_UInt glyphIndex = FT_Get_Char_Index( face, ch );

			auto iter = cachedGlyphMetrics.find( glyphIndex );
			advance = iter->second.advance;

			float xPos = (pen.x / 64.0f) + 0.5f;
			result.push_back( std::make_pair( (uint32_t)glyphIndex, vec2( xPos, curY ) ) );

			pen.x += advance.x;
			pen.y += advance.y;	
		}

		curY += lineHeight; 
	}

	return result;
}

// =================================================================================================
// SdfText::FontData
// =================================================================================================
class SdfText::FontData {
public:
	FontData( const ci::DataSourceRef &dataSource ) {
		if(! dataSource) {
			return;
		}

		mFileData = dataSource->getBuffer();
		if( ! mFileData ) {
			return;
		}

		auto fontManager = SdfTextManager::instance();
		if( nullptr != fontManager ) {
			FT_Error ftRes = FT_New_Memory_Face(
				fontManager->getLibrary(),
				reinterpret_cast<FT_Byte*>( mFileData->getData() ),
				static_cast<FT_Long>( mFileData->getSize() ),
				0,
				&mFace
			);

			if( FT_Err_Ok != ftRes ) {
				throw std::runtime_error("Failed to load font data");
			}

			fontManager->faceCreated( mFace );
		}
	}

	virtual ~FontData() {
		auto fontManager = SdfTextManager::instance();
		if( ( nullptr != fontManager ) && ( nullptr != mFace ) ) {
			fontManager->faceDestroyed( mFace );
		}
	}

	static SdfText::FontDataRef create( const ci::DataSourceRef &dataSource ) {
		SdfText::FontDataRef result = SdfText::FontDataRef( new SdfText::FontData( dataSource ) );
		return result;
	}

	FT_Face	getFace() const {
		return mFace;
	}

private:
	ci::BufferRef	mFileData;
	FT_Face			mFace = nullptr;
};

// =================================================================================================
// SdfText::Font
// =================================================================================================
SdfText::Font::Font( const std::string &name, float size )
	: mName( name ), mSize( size )
{
	auto fontManager = SdfTextManager::instance();
	if( nullptr != fontManager ) {
		SdfTextManager::FontInfo info = fontManager->getFontInfo( name );
		if( ! ci::fs::exists( info.path ) ) {
			throw std::runtime_error( info.path.string() + " does not exist" );
		}

		auto dataSource = ci::loadFile( info.path );
		loadFontData( dataSource );
	}
}

SdfText::Font::Font( DataSourceRef dataSource, float size )
	: mSize( size )
{
	if( dataSource->isFilePath() ) {
		auto fontDataSource = ci::loadFile( dataSource->getFilePath() );
		loadFontData( fontDataSource );
	}
	else {
		loadFontData( dataSource );
	}

	FT_SfntName sn = {};
	if( FT_Err_Ok == FT_Get_Sfnt_Name( mData->getFace(), TT_NAME_ID_FULL_NAME, &sn ) ) {
		// Possible Unicode name, just use filename for now
		if( sn.string_len > 0  && ( 0 == sn.string[0] ) ) {
			mName = "(Unknown)";
		}
		else {
			mName = std::string( reinterpret_cast<const char *>( sn.string ), sn.string_len );
		}
	}	
}

SdfText::Font::~Font()
{
}

void SdfText::Font::loadFontData( const ci::DataSourceRef &dataSource )
{
	mData = SdfText::FontData::create( dataSource );
	FT_Select_Charmap( mData->getFace(), FT_ENCODING_UNICODE );
	FT_Set_Char_Size( mData->getFace(), 0, mSize * 64, 0, 72 );
}

float SdfText::Font::getHeight() const
{
	float result = ( mData->getFace()->height / 64.0f );
	return result;
}

float SdfText::Font::getLeading() const
{
	float result = ( mData->getFace()->height - ( std::abs( mData->getFace()->ascender ) + std::abs( mData->getFace()->descender ) ) ) / 64.0f;
	return result;
}

float SdfText::Font::getAscent() const
{
	float result = std::fabs( mData->getFace()->ascender / 64.0f );
	return result;
}

float SdfText::Font::getDescent() const
{
	float result = std::fabs( mData->getFace()->descender / 64.0f );
	return result;
}

SdfText::Font::Glyph SdfText::Font::getGlyphIndex( size_t idx ) const
{
	return static_cast<SdfText::Font::Glyph>( idx );
}

SdfText::Font::Glyph SdfText::Font::getGlyphChar( char utf8Char ) const
{
	FT_UInt glyphIndex = FT_Get_Char_Index( mData->getFace(), static_cast<FT_ULong>( utf8Char ) );
	return static_cast<SdfText::Font::Glyph>( glyphIndex );
}

std::vector<SdfText::Font::Glyph> SdfText::Font::getGlyphs( const std::string &utf8Chars ) const
{
	std::vector<SdfText::Font::Glyph> result;
	// Convert to UTF32
	std::u32string utf32Chars = ci::toUtf32( utf8Chars );
	// Build the maps and information pieces that will be needed later
	for( const auto& ch : utf32Chars ) {
		FT_UInt glyphIndex = FT_Get_Char_Index( mData->getFace(), static_cast<FT_ULong>( ch ) );
		result.push_back( static_cast<SdfText::Font::Glyph>( glyphIndex ) );
	}
	return result;
}

FT_Face SdfText::Font::getFace() const
{
	return mData->getFace();
}

const std::vector<std::string>& SdfText::Font::getNames( bool forceRefresh )
{
	return SdfTextManager::instance()->getNames( forceRefresh );
}

SdfText::Font SdfText::Font::getDefault()
{
	return SdfTextManager::instance()->getDefault();
}

// =================================================================================================
// SdfText
// =================================================================================================
SdfText::SdfText( const SdfText::Font &font, const Format &format, const std::string &utf8Chars )
	: mFont( font ), mFormat( format )
{
	FT_Face face = font.getFace();
	if( nullptr == face ) {
		throw std::runtime_error( "null font face" );
	}

	mTextureAtlases = SdfTextManager::instance()->getTextureAtlas( face, format, utf8Chars );

	// Cache glyph metrics
	cacheGlyphMetrics();
}

SdfText::~SdfText()
{
}

SdfTextRef SdfText::create( const SdfText::Font &font, const Format &format, const std::string &supportedChars )
{
	SdfTextRef result = SdfTextRef( new SdfText( font, format, supportedChars ) );
	return result;
}

void SdfText::drawGlyphs( const SdfText::Font::GlyphMeasures &glyphMeasures, const vec2 &baselineIn, const DrawOptions &options, const std::vector<ColorA8u> &colors )
{
	const auto& textures = mTextureAtlases->mTextures;
	const auto& glyphMap = mTextureAtlases->mGlyphMap;
	const auto& sdfPadding = mTextureAtlases->mSdfPadding;
	const auto& sdfBitmapSize = mTextureAtlases->mSdfBitmapSize;

	if( textures.empty() ) {
		return;
	}

	if( ! colors.empty() ) {
		assert( glyphMeasures.size() == colors.size() );
	}

	auto shader = options.getGlslProg();
	if( ! shader ) {
		if( ! sDefaultShader ) {
			try {
				sDefaultShader = gl::GlslProg::create( kSdfVertShader, kSdfFragShader );
			}
			catch( const std::exception& e ) {
				CI_LOG_E( "sDefaultShader error: " << e.what() );
			}
		}

		shader = sDefaultShader;
	}
	ScopedTextureBind texBindScp( textures[0] );
	ScopedGlslProg glslScp( shader );

	vec2 baseline = baselineIn;

	if( ! options.getGlslProg() ) {
		shader->uniform( "uFgColor", gl::context()->getCurrentColor() );
	}

	const float fontSizeScale = mFont.getSize() / 32.0f;
	const float scale = options.getScale();
	for( size_t texIdx = 0; texIdx < textures.size(); ++texIdx ) {
		std::vector<float> verts, texCoords;
		std::vector<ColorA8u> vertColors;
		const gl::TextureRef &curTex = textures[texIdx];

		std::vector<uint32_t> indices;
		uint32_t curIdx = 0;
		GLenum indexType = GL_UNSIGNED_INT;

		if( options.getPixelSnap() ) {
			baseline = vec2( floor( baseline.x ), floor( baseline.y ) );
		}
			
		for( std::vector<std::pair<SdfText::Font::Glyph,vec2> >::const_iterator glyphIt = glyphMeasures.begin(); glyphIt != glyphMeasures.end(); ++glyphIt ) {
			SdfText::TextureAtlas::GlyphMap::const_iterator glyphInfoIt = glyphMap.find( glyphIt->first );
			if(  glyphInfoIt == glyphMap.end() ) {
				continue;
			}
				
			const auto &glyphInfo = glyphInfoIt->second;
			if( glyphInfo.mTextureIndex != texIdx ) {
				continue;
			}
			
			Rectf srcTexCoords = curTex->getAreaTexCoords( glyphInfo.mTexCoords );
			Rectf destRect( glyphInfo.mTexCoords );
			destRect.scale( fontSizeScale );
			destRect -= destRect.getUpperLeft();
			destRect.scale( scale );
			destRect += glyphIt->second * scale;
			destRect += vec2( baseline.x, baseline.y - ( sdfBitmapSize.y * fontSizeScale ) );
			vec2 originOffset = fontSizeScale * glyphInfo.mOriginOffset;
			destRect += vec2( floor( originOffset.x + 0.5f ), floor( -originOffset.y ) ) * scale;
			destRect += fontSizeScale * vec2( -sdfPadding.x, sdfPadding.y );
			if( options.getPixelSnap() ) {
				destRect -= vec2( destRect.x1 - floor( destRect.x1 ), destRect.y1 - floor( destRect.y1 ) );	
			}
			
			verts.push_back( destRect.getX2() ); verts.push_back( destRect.getY1() );
			verts.push_back( destRect.getX1() ); verts.push_back( destRect.getY1() );
			verts.push_back( destRect.getX2() ); verts.push_back( destRect.getY2() );
			verts.push_back( destRect.getX1() ); verts.push_back( destRect.getY2() );

			texCoords.push_back( srcTexCoords.getX2() ); texCoords.push_back( srcTexCoords.getY1() );
			texCoords.push_back( srcTexCoords.getX1() ); texCoords.push_back( srcTexCoords.getY1() );
			texCoords.push_back( srcTexCoords.getX2() ); texCoords.push_back( srcTexCoords.getY2() );
			texCoords.push_back( srcTexCoords.getX1() ); texCoords.push_back( srcTexCoords.getY2() );
			
			if( ! colors.empty() ) {
				for( int i = 0; i < 4; ++i ) {
					vertColors.push_back( colors[glyphIt-glyphMeasures.begin()] );
				}
			}

			indices.push_back( curIdx + 0 ); indices.push_back( curIdx + 1 ); indices.push_back( curIdx + 2 );
			indices.push_back( curIdx + 2 ); indices.push_back( curIdx + 1 ); indices.push_back( curIdx + 3 );
			curIdx += 4;
		}
		
		if( curIdx == 0 ) {
			continue;
		}
		
		curTex->bind();
		auto ctx = gl::context();
		size_t dataSize = (verts.size() + texCoords.size()) * sizeof(float) + vertColors.size() * sizeof(ColorA8u);
		gl::ScopedVao vaoScp( ctx->getDefaultVao() );
		ctx->getDefaultVao()->replacementBindBegin();
		VboRef defaultElementVbo = ctx->getDefaultElementVbo( indices.size() * sizeof(curIdx) );
		VboRef defaultArrayVbo = ctx->getDefaultArrayVbo( dataSize );

		ScopedBuffer vboArrayScp( defaultArrayVbo );
		ScopedBuffer vboElScp( defaultElementVbo );

		size_t dataOffset = 0;
		int posLoc = shader->getAttribSemanticLocation( geom::Attrib::POSITION );
		if( posLoc >= 0 ) {
			enableVertexAttribArray( posLoc );
			vertexAttribPointer( posLoc, 2, GL_FLOAT, GL_FALSE, 0, (void*)0 );
			defaultArrayVbo->bufferSubData( dataOffset, verts.size() * sizeof(float), verts.data() );
			dataOffset += verts.size() * sizeof(float);
		}
		int texLoc = shader->getAttribSemanticLocation( geom::Attrib::TEX_COORD_0 );
		if( texLoc >= 0 ) {
			enableVertexAttribArray( texLoc );
			vertexAttribPointer( texLoc, 2, GL_FLOAT, GL_FALSE, 0, (void*)dataOffset );
			defaultArrayVbo->bufferSubData( dataOffset, texCoords.size() * sizeof(float), texCoords.data() );
			dataOffset += texCoords.size() * sizeof(float);
		}
		if( ! vertColors.empty() ) {
			int colorLoc = shader->getAttribSemanticLocation( geom::Attrib::COLOR );
			if( colorLoc >= 0 ) {
				enableVertexAttribArray( colorLoc );
				vertexAttribPointer( colorLoc, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, (void*)dataOffset );
				defaultArrayVbo->bufferSubData( dataOffset, vertColors.size() * sizeof(ColorA8u), vertColors.data() );
				dataOffset += vertColors.size() * sizeof(ColorA8u);				
			}
		}

		defaultElementVbo->bufferSubData( 0, indices.size() * sizeof(curIdx), indices.data() );
		ctx->getDefaultVao()->replacementBindEnd();
		gl::setDefaultShaderVars();
		ctx->drawElements( GL_TRIANGLES, (GLsizei)indices.size(), indexType, 0 );
	}
}

void SdfText::drawGlyphs( const SdfText::Font::GlyphMeasures &glyphMeasures, const Rectf &clip, vec2 offset, const DrawOptions &options, const std::vector<ColorA8u> &colors )
{
	const auto& textures = mTextureAtlases->mTextures;
	const auto& glyphMap = mTextureAtlases->mGlyphMap;
	const auto& sdfPadding = mTextureAtlases->mSdfPadding;
	const auto& sdfBitmapSize = mTextureAtlases->mSdfBitmapSize;

	if( textures.empty() ) {
		return;
	}

	if( ! colors.empty() ) {
		assert( glyphMeasures.size() == colors.size() );
	}

	auto shader = options.getGlslProg();
	if( ! shader ) {
		if( ! sDefaultShader ) {
			try {
				sDefaultShader = gl::GlslProg::create( kSdfVertShader, kSdfFragShader );
			}
			catch( const std::exception& e ) {
				CI_LOG_E( "sDefaultShader error: " << e.what() );
			}
		}

		shader = sDefaultShader;
	}
	ScopedTextureBind texBindScp( textures[0] );
	ScopedGlslProg glslScp( shader );

	const float fontSizeScale = mFont.getSize() / 32.0f;
	const float scale = options.getScale();
	for( size_t texIdx = 0; texIdx < textures.size(); ++texIdx ) {
		std::vector<float> verts, texCoords;
		std::vector<ColorA8u> vertColors;
		const gl::TextureRef &curTex = textures[texIdx];

		std::vector<uint32_t> indices;
		uint32_t curIdx = 0;
		GLenum indexType = GL_UNSIGNED_INT;

		if( options.getPixelSnap() ) {
			offset = vec2( floor( offset.x ), floor( offset.y ) );
		}

		for( std::vector<std::pair<Font::Glyph,vec2> >::const_iterator glyphIt = glyphMeasures.begin(); glyphIt != glyphMeasures.end(); ++glyphIt ) {
			SdfText::TextureAtlas::GlyphMap::const_iterator glyphInfoIt = glyphMap.find( glyphIt->first );
			if( glyphInfoIt == glyphMap.end() ) {
				continue;
			}
				
			const auto &glyphInfo = glyphInfoIt->second;
			if( glyphInfo.mTextureIndex != texIdx ) {
				continue;
			}

			Rectf srcTexCoords = curTex->getAreaTexCoords( glyphInfo.mTexCoords );
			Rectf destRect( glyphInfo.mTexCoords );
			destRect.scale( fontSizeScale );
			destRect -= destRect.getUpperLeft();
			destRect.scale( scale );
			destRect += glyphIt->second * scale;
			destRect += vec2( offset.x, offset.y );
			vec2 originOffset = fontSizeScale * glyphInfo.mOriginOffset;
			destRect += vec2( floor( originOffset.x + 0.5f ), floor( -originOffset.y ) ) * scale;
			destRect += fontSizeScale * vec2( -sdfPadding.x, -sdfPadding.y );
			if( options.getPixelSnap() ) {
				destRect -= vec2( destRect.x1 - floor( destRect.x1 ), destRect.y1 - floor( destRect.y1 ) );	
			}

			// clip
			Rectf clipped( destRect );
			if( options.getClipHorizontal() ) {
				clipped.x1 = std::max( destRect.x1, clip.x1 );
				clipped.x2 = std::min( destRect.x2, clip.x2 );
			}
			if( options.getClipVertical() ) {
				clipped.y1 = std::max( destRect.y1, clip.y1 );
				clipped.y2 = std::min( destRect.y2, clip.y2 );
			}
			
			if( clipped.x1 >= clipped.x2 || clipped.y1 >= clipped.y2 ) {
				continue;
			}

			verts.push_back( clipped.getX2() ); verts.push_back( clipped.getY1() );
			verts.push_back( clipped.getX1() ); verts.push_back( clipped.getY1() );
			verts.push_back( clipped.getX2() ); verts.push_back( clipped.getY2() );
			verts.push_back( clipped.getX1() ); verts.push_back( clipped.getY2() );

			vec2 coordScale = vec2( srcTexCoords.getWidth() / destRect.getWidth(), srcTexCoords.getHeight() / destRect.getHeight() );
			srcTexCoords.x1 = srcTexCoords.x1 + ( clipped.x1 - destRect.x1 ) * coordScale.x;
			srcTexCoords.x2 = srcTexCoords.x1 + ( clipped.x2 - clipped.x1  ) * coordScale.x;
			srcTexCoords.y1 = srcTexCoords.y1 + ( clipped.y1 - destRect.y1 ) * coordScale.y;
			srcTexCoords.y2 = srcTexCoords.y1 + ( clipped.y2 - clipped.y1  ) * coordScale.y;

			texCoords.push_back( srcTexCoords.getX2() ); texCoords.push_back( srcTexCoords.getY1() );
			texCoords.push_back( srcTexCoords.getX1() ); texCoords.push_back( srcTexCoords.getY1() );
			texCoords.push_back( srcTexCoords.getX2() ); texCoords.push_back( srcTexCoords.getY2() );
			texCoords.push_back( srcTexCoords.getX1() ); texCoords.push_back( srcTexCoords.getY2() );

			if( ! colors.empty() ) {
				for( int i = 0; i < 4; ++i ) {
					vertColors.push_back( colors[glyphIt-glyphMeasures.begin()] );
				}
			}
			
			indices.push_back( curIdx + 0 ); indices.push_back( curIdx + 1 ); indices.push_back( curIdx + 2 );
			indices.push_back( curIdx + 2 ); indices.push_back( curIdx + 1 ); indices.push_back( curIdx + 3 );
			curIdx += 4;
		}
		
		if( curIdx == 0 ) {
			continue;
		}
		
		curTex->bind();
		auto ctx = gl::context();
		size_t dataSize = (verts.size() + texCoords.size()) * sizeof(float) + vertColors.size() * sizeof(ColorA8u);
		gl::ScopedVao vaoScp( ctx->getDefaultVao() );
		ctx->getDefaultVao()->replacementBindBegin();
		VboRef defaultElementVbo = ctx->getDefaultElementVbo( indices.size() * sizeof(curIdx) );
		VboRef defaultArrayVbo = ctx->getDefaultArrayVbo( dataSize );

		ScopedBuffer vboArrayScp( defaultArrayVbo );
		ScopedBuffer vboElScp( defaultElementVbo );

		size_t dataOffset = 0;
		int posLoc = shader->getAttribSemanticLocation( geom::Attrib::POSITION );
		if( posLoc >= 0 ) {
			enableVertexAttribArray( posLoc );
			vertexAttribPointer( posLoc, 2, GL_FLOAT, GL_FALSE, 0, (void*)0 );
			defaultArrayVbo->bufferSubData( dataOffset, verts.size() * sizeof(float), verts.data() );
			dataOffset += verts.size() * sizeof(float);
		}
		int texLoc = shader->getAttribSemanticLocation( geom::Attrib::TEX_COORD_0 );
		if( texLoc >= 0 ) {
			enableVertexAttribArray( texLoc );
			vertexAttribPointer( texLoc, 2, GL_FLOAT, GL_FALSE, 0, (void*)dataOffset );
			defaultArrayVbo->bufferSubData( dataOffset, texCoords.size() * sizeof(float), texCoords.data() );
			dataOffset += texCoords.size() * sizeof(float);
		}
		if( ! vertColors.empty() ) {
			int colorLoc = shader->getAttribSemanticLocation( geom::Attrib::COLOR );
			if( colorLoc >= 0 ) {
				enableVertexAttribArray( colorLoc );
				vertexAttribPointer( colorLoc, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, (void*)dataOffset );
				defaultArrayVbo->bufferSubData( dataOffset, vertColors.size() * sizeof(ColorA8u), vertColors.data() );
				dataOffset += vertColors.size() * sizeof(ColorA8u);				
			}
		}

		defaultElementVbo->bufferSubData( 0, indices.size() * sizeof(curIdx), indices.data() );
		ctx->getDefaultVao()->replacementBindEnd();
		gl::setDefaultShaderVars();
		ctx->drawElements( GL_TRIANGLES, (GLsizei)indices.size(), indexType, 0 );
	}
}

void SdfText::drawString( const std::string &str, const vec2 &baseline, const DrawOptions &options )
{
	SdfTextBox tbox = SdfTextBox().font( mFont ).text( str ).size( SdfTextBox::GROW, SdfTextBox::GROW ).ligate( options.getLigate() );
	SdfText::Font::GlyphMeasures glyphMeasures = tbox.measureGlyphs( mCachedGlyphMetrics, options );
	drawGlyphs( glyphMeasures, baseline, options );
}

void SdfText::drawString( const std::string &str, const Rectf &fitRect, const vec2 &offset, const DrawOptions &options )
{
	SdfTextBox tbox = SdfTextBox().font( mFont ).text( str ).size( SdfTextBox::GROW, fitRect.getHeight() ).ligate( options.getLigate() );
	SdfText::Font::GlyphMeasures glyphMeasures = tbox.measureGlyphs( mCachedGlyphMetrics, options );
	drawGlyphs( glyphMeasures, fitRect, fitRect.getUpperLeft() + offset, options );	
}

void SdfText::drawStringWrapped( const std::string &str, const Rectf &fitRect, const vec2 &offset, const DrawOptions &options )
{
	SdfTextBox tbox = SdfTextBox().font( mFont ).text( str ).size( fitRect.getWidth(), fitRect.getHeight() ).ligate( options.getLigate() );
	SdfText::Font::GlyphMeasures glyphMeasures = tbox.measureGlyphs( mCachedGlyphMetrics, options );
	drawGlyphs( glyphMeasures, fitRect.getUpperLeft() + offset, options );
}

vec2 SdfText::measureString( const std::string &str, const DrawOptions &options ) const
{
	const SdfText::TextureAtlas::GlyphMap& mGlyphMap = mTextureAtlases->mGlyphMap;
	SdfTextBox tbox = SdfTextBox().font( mFont ).text( str ).size( SdfTextBox::GROW, SdfTextBox::GROW ).ligate( options.getLigate() );
	SdfText::Font::GlyphMeasures glyphMeasures = tbox.measureGlyphs( mCachedGlyphMetrics, options );
	if( ! glyphMeasures.empty() ) {
		vec2 result = glyphMeasures.back().second;
		SdfText::TextureAtlas::GlyphMap::const_iterator glyphInfoIt = mGlyphMap.find( glyphMeasures.back().first );
		if( glyphInfoIt != mGlyphMap.end() ) {
			result += glyphInfoIt->second.mOriginOffset + vec2( glyphInfoIt->second.mTexCoords.getSize() );
		}
		return result;
	}
	else {
		return vec2();
	}
}

std::vector<std::pair<SdfText::Font::Glyph, vec2>> SdfText::getGlyphPlacements( const std::string &str, const DrawOptions &options ) const
{
	SdfTextBox tbox = SdfTextBox().font( mFont ).text( str ).size( SdfTextBox::GROW, SdfTextBox::GROW ).ligate( options.getLigate() );
	return tbox.measureGlyphs( mCachedGlyphMetrics, options );
}

std::vector<std::pair<SdfText::Font::Glyph, vec2>> SdfText::getGlyphPlacements( const std::string &str, const Rectf &fitRect, const DrawOptions &options ) const
{
	SdfTextBox tbox = SdfTextBox().font( mFont ).text( str ).size( SdfTextBox::GROW, fitRect.getHeight() ).ligate( options.getLigate() );
	return tbox.measureGlyphs( mCachedGlyphMetrics, options );
}

std::vector<std::pair<SdfText::Font::Glyph, vec2>> SdfText::getGlyphPlacementsWrapped( const std::string &str, const Rectf &fitRect, const DrawOptions &options ) const
{
	SdfTextBox tbox = SdfTextBox().font( mFont ).text( str ).size( fitRect.getWidth(), fitRect.getHeight() ).ligate( options.getLigate() );
	return tbox.measureGlyphs( mCachedGlyphMetrics, options );
}

std::string SdfText::defaultChars() 
{ 
	return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz1234567890().?!,:;'\"&*=+-/\\@#_[]<>%^llflfiphrids\303\251\303\241\303\250\303\240"; 
}

void SdfText::cacheGlyphMetrics()
{
	FT_Face face = mFont.getFace();
	for( const auto it : mTextureAtlases->mCharMap ) {
		SdfText::Font::Glyph glyphIndex = it.second;
		FT_Load_Glyph( face, glyphIndex, FT_LOAD_DEFAULT );
		FT_GlyphSlot slot = face->glyph;
		SdfText::Font::GlyphMetrics glyphMetrics;
		glyphMetrics.advance = ivec2( slot->advance.x, slot->advance.y );
		mCachedGlyphMetrics[glyphIndex] = glyphMetrics;
	}
}

uint32_t SdfText::getNumTextures() const
{
	return static_cast<uint32_t>( mTextureAtlases->mTextures.size() );
}

const gl::TextureRef& SdfText::getTexture(uint32_t n) const
{
	return mTextureAtlases->mTextures[static_cast<size_t>( n )];
}

}} // namespace cinder::gl